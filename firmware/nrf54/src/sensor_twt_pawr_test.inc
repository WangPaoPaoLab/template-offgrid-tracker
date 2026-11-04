/* Factory-only application-thread experiment. RX callbacks only enqueue.
 * Included after raw command helpers to keep the production sensor path small. */
static uint64_t sensor_twt_now_us(void)
{
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

static bool sensor_twt_ps_contract(uint16_t command_id)
{
    struct morse_cmd_req_li_sleep li = MORSE_COMMAND_INIT(li,
        MORSE_CMD_ID_LI_SLEEP, mesh_runtime.umac.vif_id,
        .listen_interval = htole32(2));
    struct morse_cmd_req_get_set_generic_param non_tim = MORSE_COMMAND_INIT(non_tim,
        MORSE_CMD_ID_GET_SET_GENERIC_PARAM, mesh_runtime.umac.vif_id,
        .param_id = htole32(MORSE_CMD_PARAM_ID_NON_TIM_MODE),
        .action = htole32(MORSE_CMD_PARAM_ACTION_SET), .value = htole32(1));
    struct { struct morse_cmd_resp resp; uint8_t extra[112]; } reply = {0};
    uint32_t len = sizeof(reply);
    enum mmwlan_status sdk = mmdrv_execute_command(
        command_id == MORSE_CMD_ID_LI_SLEEP ? (uint8_t *)&li : (uint8_t *)&non_tim,
        (uint8_t *)&reply, &len);
    bool valid = (sdk == MMWLAN_SUCCESS || sdk == MMWLAN_COMMAND_ERROR) &&
        len >= sizeof(reply.resp) && len <= sizeof(reply) &&
        le16toh(reply.resp.hdr.message_id) == command_id &&
        le16toh(reply.resp.hdr.vif_id) == mesh_runtime.umac.vif_id;
    LOG_INF("PAWR_TWT PS_CONTRACT cmd=0x%04x sdk=%d valid=%u fw=%d bytes=%u",
        command_id, sdk, valid,
        (int)(valid ? (int32_t)le32toh(reply.resp.status) : INT32_MIN), len);
    return valid && sdk == MMWLAN_SUCCESS && le32toh(reply.resp.status) == 0;
}

static bool sensor_twt_send_to(const struct edgez_pawr_twt *msg, const uint8_t peer[6])
{
    uint8_t payload[EDGEZ_PAWR_TWT_LENGTH];
    uint8_t packet[EDGEZ_PAWR_HEADER_LENGTH + EDGEZ_PAWR_TWT_TRAILER_LENGTH];
    uint8_t frame[EDGEZ_PAWR_ACTION_HEADER_LENGTH + sizeof(packet)];
    size_t packet_len, frame_len;
    if (msg->kind == EDGEZ_PAWR_TWT_HIT) {
        if (!edgez_pawr_encode_response(packet, sizeof(packet),
                edgez_pawr_payload_hash(sensor_remote_wake_payload, sizeof(sensor_remote_wake_payload)),
                mesh_runtime.config.compressed_ssid, &packet_len) ||
            !edgez_pawr_twt_append(packet, sizeof(packet), &packet_len, msg)) return false;
    } else if (!edgez_pawr_twt_encode(payload, sizeof(payload), msg) ||
        !edgez_pawr_encode_typed_request(packet, sizeof(packet), EDGEZ_PAWR_REQUEST_TWT_CONTROL,
            ++pawr_request_id, 0, mesh_runtime.config.compressed_ssid,
            payload, sizeof(payload), &packet_len)) return false;
    if (!edgez_pawr_encode_action_frame(frame, sizeof(frame), pawr_sequence_number++,
            mesh_runtime.radio_info.mac_addr, peer,
            packet, packet_len, &frame_len)) return false;
    const struct edgez_umac_tx_request tx = {
        .token = ++pawr_tx_token, .frame = frame, .frame_length = frame_len,
        .frame_type = EDGEZ_UMAC_TX_MANAGEMENT, .tid = 8, .mcs = 0,
        .attempts = 1, .bandwidth_mhz = 1, .no_ack = true, .immediate_status = true,
    };
    bool feedback = msg->kind == EDGEZ_PAWR_TWT_HIT;
    if (feedback) {
        k_sem_reset(&sensor_twt_tx_done);
        atomic_clear(&sensor_twt_tx_failed);
        sensor_twt_tx_token = tx.token;
        atomic_set(&sensor_twt_tx_armed, 1);
    }
    bool submitted = edgez_umac_transmit(&mesh_runtime.umac, &tx) == EDGEZ_UMAC_OK;
    if (!feedback) return submitted;
    /* Submission is asynchronous. Reconfigure PS only once this ACK has
     * completed, never from the driver callback or while its TX is queued. */
    bool completed = submitted && k_sem_take(&sensor_twt_tx_done, K_MSEC(200)) == 0;
    atomic_clear(&sensor_twt_tx_armed);
    bool failed = atomic_get(&sensor_twt_tx_failed);
    LOG_INF("PAWR_TWT ACK_TX token=%u submitted=%u completed=%u failed=%u",
        (unsigned)tx.token, submitted, completed, failed);
    return completed && !failed;
}

static bool sensor_twt_send(const struct edgez_pawr_twt *msg)
{
    return sensor_twt_send_to(msg, sensor_probe_peer_mac);
}

static bool sensor_twt_wait(const struct edgez_pawr_twt *offer, uint8_t kind,
                            struct sensor_twt_event *event, int timeout_ms)
{
    int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline) {
        if (k_msgq_get(&sensor_twt_queue, event, K_MSEC(deadline - k_uptime_get())) != 0)
            return false;
        if (event->msg.kind == kind && memcmp(event->peer, sensor_probe_peer_mac, 6) == 0 &&
            edgez_pawr_twt_same_schedule(offer, &event->msg) &&
            event->msg.round_trip_us == offer->round_trip_us && !event->msg.sequence)
            return true;
    }
    return false;
}

static bool sensor_twt_pawr_test(void)
{
    /* Re-enter on the normal sensor retry cycle after a disconnect. A
     * one-shot boot latch would permanently lose USER wake after expiry. */
    bool installed = false;
    struct sensor_twt_event event;
    struct edgez_pawr_twt offer = {0};
    unsigned inside = 0, outside = 0;
    uint64_t offered_at = 0;
    uint64_t led_until = 0;
    uint64_t last_probe_us = 0;
    bool host_released = false;
    k_spinlock_key_t key = k_spin_lock(&sensor_pawr_lock);
    bool seeded = sensor_twt_seed_valid;
    if (seeded) event = sensor_twt_seed;
    sensor_twt_seed_valid = false;
    k_spin_unlock(&sensor_pawr_lock, key);
    if (!seeded) {
        LOG_INF("PAWR_TWT no session in PAwR ACK; defer until next sensor cycle");
        return false;
    }
    memcpy(sensor_probe_peer_mac, event.peer, 6);
    atomic_set(&sensor_probe_peer_seen, 1);
    offer = (struct edgez_pawr_twt) {
        .kind = EDGEZ_PAWR_TWT_OFFER, .session = event.msg.session,
        .interval_us = 2000000, .duration_us = 50176, .lease_ms = 0,
    };
    LOG_INF("PAWR_TWT session=%u from PAwR ACK; no discovery", offer.session);
    if (mmdrv_set_wake_enabled(true) != MMWLAN_SUCCESS ||
        !sensor_probe_long_sleep(false) || !sensor_probe_config_ps(false, false)) goto done;
    k_msgq_purge(&sensor_twt_queue);
    /* The SDK programs LI_SLEEP at interface creation, then non-TIM after
     * association. Two beacon intervals are 2.048 s with our 1000-TU BSS;
     * this does not change the separately negotiated 2.000 s TWT agreement. */
    if (!sensor_twt_ps_contract(MORSE_CMD_ID_LI_SLEEP)) goto done;
    for (uint16_t state = 1; state <= 4; ++state) {
        if (!sensor_probe_sta_state(state)) goto done;
        if (state == 3 && !sensor_twt_ps_contract(MORSE_CMD_ID_GET_SET_GENERIC_PARAM)) goto done;
    }
    /* Match mmdrv_set_chip_power_save_enabled(): delegate dynamic PS to
     * firmware. Linux uses PS_NULLFUNC_STACK when offload is disabled;
     * this stateless host does not implement that PM-null state machine. */
    if (!sensor_probe_config_ps(true, true)) goto done;
    /* Clock mapping must be supported and advancing before any install. */
    uint64_t previous;
    if (!sensor_probe_get_tsf(&previous)) goto done;
    k_msleep(20);
    bool accepted = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        offered_at = sensor_twt_now_us();
        if (!sensor_probe_get_tsf(&offer.reference_tsf) || offer.reference_tsf <= previous) goto done;
        previous = offer.reference_tsf;
        offer.target_tsf = offer.reference_tsf + 3000000;
        offer.round_trip_us = 0;
        LOG_INF("PAWR_TWT offer session=%u attempt=%u target=%llu interval_us=%u duration_us=%u",
            offer.session, attempt, (unsigned long long)offer.target_tsf,
            offer.interval_us, offer.duration_us);
        if (!sensor_twt_send(&offer)) goto done;
        if (!sensor_twt_wait(&offer, EDGEZ_PAWR_TWT_ACCEPT, &event, 400)) continue;
        uint64_t rtt = event.received_us - offered_at;
        if (!rtt || rtt > 30000) {
            LOG_WRN("PAWR_TWT timing rejected round_trip_us=%llu", (unsigned long long)rtt);
            continue;
        }
        offer.round_trip_us = (uint32_t)rtt;
        accepted = true;
        break;
    }
    if (!accepted) { LOG_ERR("PAWR_TWT accept timeout or timing uncertainty"); goto done; }
    /* Announced implicit flow 0, exponent 5: 62500 << 5 = 2 seconds. */
    if (!sensor_probe_twt_at(MORSE_CMD_ID_TWT_AGREEMENT_VALIDATE, 0x1420, offer.target_tsf)) goto done;
    installed = sensor_probe_twt_at(MORSE_CMD_ID_TWT_AGREEMENT_INSTALL, 0x1420, offer.target_tsf);
    if (!installed) goto done;
    offer.kind = EDGEZ_PAWR_TWT_COMMIT;
    bool ready = false;
    for (unsigned retry = 0; retry < 3; ++retry) {
        if (!sensor_twt_send(&offer)) goto done;
        if (sensor_twt_wait(&offer, EDGEZ_PAWR_TWT_READY, &event, 200)) { ready = true; break; }
    }
    if (!ready) { LOG_ERR("PAWR_TWT ready timeout"); goto done; }
    /* Release host wake after setup. Only actual PAwR USER traffic supplies
     * timing samples; silence does not expire the installed agreement. */
    sensor_led_set_awake(false);
    uint64_t expires = offer.lease_ms ? offered_at + 3000000 + (uint64_t)offer.lease_ms * 1000 : UINT64_MAX;
    uint64_t release_at = sensor_twt_now_us();
    LOG_INF("PAWR_TWT ACTIVE session=%u round_trip_us=%u lease_ms=%u monitor=0 scan=0 long_sleep=0 awake_control_ms=0 calibration=piggyback",
        offer.session, offer.round_trip_us, offer.lease_ms);
    for (;;) {
        uint64_t now = sensor_twt_now_us();
        if (led_until && now >= led_until) {
            sensor_led_set_awake(false);
            LOG_INF("PAWR_TWT LED off; five-second USER wake window ended");
            led_until = 0;
        }
        if (now >= expires) break;
        if (!host_released && now >= release_at) {
            if (mmdrv_set_wake_enabled(false) != MMWLAN_SUCCESS) goto done;
            sensor_halow_sleep_allowed = host_released = true;
            LOG_INF("PAWR_TWT host wake released; subsequent calibration runs only after RX");
        }
        uint64_t deadline = led_until && led_until < expires ? led_until : expires;
        if (!host_released && release_at < deadline) deadline = release_at;
        if (k_msgq_get(&sensor_twt_queue, &event, (deadline == UINT64_MAX ? K_FOREVER : K_USEC(deadline - now))) != 0) continue;
        if (memcmp(event.peer, sensor_probe_peer_mac, 6) ||
            !edgez_pawr_twt_same_schedule(&offer, &event.msg) ||
            event.msg.round_trip_us != offer.round_trip_us) continue;
        if (event.msg.kind == EDGEZ_PAWR_TWT_CANCEL) break;
        if (event.msg.kind != EDGEZ_PAWR_TWT_PROBE) continue;
        if (event.msg.echo_host_us <= last_probe_us) continue;
        last_probe_us = event.msg.echo_host_us;
        if (offer.lease_ms) expires = sensor_twt_now_us() + (uint64_t)offer.lease_ms * 1000;
        if ((event.msg.sequence & 0x7fffffffU) >= 30U ||
            !(event.msg.sequence & 0x80000000U)) continue;
        if (event.msg.sequence & 0x80000000U) {
            sensor_led_set_awake(true);
            led_until = sensor_twt_now_us() + 5000000;
            LOG_INF("PAWR_TWT LED on P2.05; USER wake sequence=%u duration_ms=5000", event.msg.sequence);
        }
        bool off_window = event.msg.sequence & 1U;
        if (off_window) ++outside; else ++inside;
        LOG_INF("PAWR_TWT RX sequence=%u planned=%s host_us=%llu host_released=%u", event.msg.sequence,
            off_window ? "outside" : "inside", (unsigned long long)event.received_us, host_released);
        {
            if (host_released && mmdrv_set_wake_enabled(true) != MMWLAN_SUCCESS) goto done;
            uint64_t before = sensor_twt_now_us(), tsf;
            if (!sensor_probe_get_tsf(&tsf)) goto done;
            uint64_t after = sensor_twt_now_us();
            uint64_t rx_tsf = tsf - ((before + after) / 2 - event.received_us);
            if (rx_tsf >= offer.target_tsf) {
                uint32_t phase = (rx_tsf - offer.target_tsf) % offer.interval_us;
                uint32_t expected = offer.duration_us / 2 + (off_window ? offer.interval_us / 2 : 0);
                int32_t error = (int32_t)phase - (int32_t)expected;
                if (error > (int32_t)offer.interval_us / 2) error -= offer.interval_us;
                if (error < -(int32_t)offer.interval_us / 2) error += offer.interval_us;
                LOG_INF("PAWR_TWT PACE sequence=%u radio_phase_us=%u expected_us=%u error_us=%d query_span_us=%llu",
                    event.msg.sequence, phase, expected, error, (unsigned long long)(after - before));
                uint64_t processing = sensor_twt_now_us() - event.received_us;
                if (after - before <= 10000 && processing > 0 && processing <= 10000 &&
                    error >= -20000 && error <= 20000) {
                    struct edgez_pawr_twt feedback = event.msg;
                    feedback.kind = EDGEZ_PAWR_TWT_HIT;
                    feedback.phase_error_us = error;
                    feedback.processing_us = (uint32_t)processing;
                    if (!sensor_twt_send(&feedback)) goto done;
                }

            }
        }
        if (host_released) {
            enum mmwlan_status released = mmdrv_set_wake_enabled(false);
            LOG_INF("PAWR_TWT POST_TX host_release_sdk=%d", released);
            if (released != MMWLAN_SUCCESS) goto done;
        }
        /* Timing feedback piggybacks on the USER request hash ACK. Current is measured externally;
         * no GPIO, BUSY or packet counters infer the RF sleep state. */
    }
    LOG_INF("PAWR_TWT EXPIRED received_inside=%u received_outside=%u", inside, outside);
 done:
    sensor_led_set_awake(false);
    (void)mmdrv_set_wake_enabled(true);
    sensor_halow_sleep_allowed = false;
    if (installed) (void)sensor_probe_twt_at(MORSE_CMD_ID_TWT_AGREEMENT_REMOVE, 0x1420, 0);
    if (offer.target_tsf) { offer.kind = EDGEZ_PAWR_TWT_CANCEL; (void)sensor_twt_send(&offer); k_msleep(20); }
    atomic_clear(&sensor_remote_wake_pending);
    k_sem_reset(&sensor_remote_wake_sem);
    LOG_INF("PAWR_TWT DONE; full radio shutdown clears experimental station/TWT state");
    return false;
}
