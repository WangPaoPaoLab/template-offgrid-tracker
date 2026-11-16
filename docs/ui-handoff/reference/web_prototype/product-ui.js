/* Interactive presentation with browser-local media. No BLE, MQTT, cloud or hardware commands. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const copy = value => JSON.parse(JSON.stringify(value));
  const catalog = window.EdgezRadioCatalog;
  const STORE = 'edgez.product-board.v1';
  let saved = {};
  try { saved = JSON.parse(localStorage.getItem(STORE) || '{}') || {}; } catch {}
  const M=window.EdgezAlignment,products=M.createProducts();
  const initialRadio=copy(M.radioDefaults),freshRadio=M.transaction;
  const baseTime = Date.now();
  const nodes = M.enrichNodes([
    ['n01','023','collar',3.98,-65,31.2310,121.4740,20],
    ['n02','026','collar',3.82,-72,31.2320,121.4770,42],
    ['n03','031','collar',3.29,-83,31.2370,121.4810,65],
    ['n04','035','collar',3.91,-69,31.2290,121.4730,83],
    ['n05','042','collar',3.64,-88,31.2260,121.4700,95],
    ['n06','047','collar',3.71,-76,31.2300,121.4790,900],
    ['n07','051','collar',3.86,-71,null,null,35],
    ['n08','058','collar',null,null,31.2280,121.4750,110],
  ],baseTime);
  const peers = [{id:'p1',zh:'牧场管理员',en:'Ranch manager',online:true},{id:'p2',zh:'西区巡护员',en:'West patrol',online:true},{id:'p3',zh:'营地队友',en:'Camp teammate',online:false}].map((p,i)=>({...p,nodeNumString:(9007199254742001n+BigInt(i)).toString()}));
  const defaultFence = {gatewayId:'gateway-01',syncState:'local',executionTarget:'unassigned',revision:0,id:'f1',name:'牧区 A',nameKey:'m259',shape:'circle',center:{latitude:31.231,longitude:121.475},radiusMeters:500,radiusXMeters:600,radiusYMeters:350,widthMeters:1000,heightMeters:700,rotationDegrees:0,vertices:[],deviceIds:[],enterAlert:false,exitAlert:true,enabled:true};
  const S = {
    reviewMode:Boolean(document.body.hasAttribute?.('data-review')),appearance:['light','dark'].includes(saved.appearance)?saved.appearance:'system',
    lang:window.EdgezI18n.has(saved.language)?saved.language:'zh',route:'home',homeArea:'all',lifecycle:1,links:{gateway:false,radio:false},collarTab:'nodes',chatTab:'channels',thread:null,nodeId:null,selectedMapNode:null,
    search:'',filter:'all',modal:null,error:'',fenceDraft:null,radioDraft:null,target:'gateway',map:{zoom:1,x:0,y:0},busy:false,
    fences:Array.isArray(saved.fences)&&saved.fences.every(f=>f&&typeof f.id==='string'&&['circle','oval','rectangle','polygon'].includes(f.shape)&&f.center&&Number.isFinite(f.center.latitude)&&Number.isFinite(f.center.longitude)&&Array.isArray(f.deviceIds)&&Array.isArray(f.vertices))?saved.fences:[],
    activeGateway:'gateway',activeChat:'radio',proposal:false,scenario:'normal',renameScope:'local',renameDraft:'',renameStatus:'idle',productRenameDraft:'',resetEmail:'',resetSent:false,authBusy:false,radioAdvanced:false,radioReadBusy:false,views:{},packages:document.body.hasAttribute?.('data-review')&&Array.isArray(saved.packages)?saved.packages:[],mapEpoch:0,ackBusy:null,
    networkProfile:null,networkBack:'profile',pendingApply:null,preferences:{units:saved.preferences?.units==='imperial'?'imperial':'metric',notifications:Object.fromEntries(['messages','fences','sound','vibration'].map(k=>[k,saved.preferences?.notifications?.[k]!==false]))},notificationEvents:[],
    acknowledged:[],radio:Object.fromEntries(products.map(p=>[p.key,p.config])),account:null,loginMode:'code',accountEmail:'',accountPassword:'',accountCode:'',showAccountPassword:false,otpEmail:'',scanStatus:'idle',pendingDevice:null,bindings:{gateway:null,radio:null},caption:'',identity:{name:saved.identityName||'',id:'demo-user-8c42',nodeId:'!a8f4c201',publicKey:'DEMO-PUBLIC-KEY'},messages:{},composerDrafts:{},storageError:false,
  };
  const productByKey = key => products.find(p=>p.key===key||p.id===key);
  const currentGateway = () => productByKey(S.activeGateway);
  const activeTarget = () => S.route==='chat'?S.activeChat:S.activeGateway;
  const capability = (key,target=activeTarget()) => M.capabilities(productByKey(target),S.proposal)[key]===true;
  for(const p of products){if(typeof saved.productNames?.[p.id]==='string')p.alias=saved.productNames[p.id].slice(0,40);if(saved.knownGateways?.includes(p.id)){p.cache.known=true;p.cache.status='cached';}if(p.key==='gateway2'&&saved.extraGateway)p.present=true;}
  for(const n of nodes){if(typeof saved.nodeNames?.[n.id]==='string'&&(saved.schemaVersion===2||saved.nodeNames[n.id]!==n.hardwareName))n.alias=saved.nodeNames[n.id].slice(0,40);const old=saved.nodeSamples?.[n.id];if(old?.observations&&Number.isFinite(old.lastSeenAt)){n.lastSeenAt=old.lastSeenAt;n.observations=copy(old.observations);if(typeof old.name==='string')n.name=old.name;}}
  // Legacy booleans are compatibility accessors, never the connection source of truth.
  S.links={};for(const p of products)Object.defineProperty(S.links,p.key,{enumerable:true,get:()=>M.ready(p),set:value=>{p.connections.ble=value?'ready':'disconnected';}});
  const outcome = kind => ({message:$('demo-send')?.value||'delivered',config:$('demo-apply')?.value||'success',identify:$('demo-identify')?.value||'known',nodes:S.scenario==='denied'?'denied':S.scenario==='partial'?'failed':'success',rename:$('demo-operation')?.value||'success',fence:$('demo-operation')?.value||'success',auth:$('demo-operation')?.value||'success',map:$('demo-operation')?.value||'success'})[kind];
  const adapter=M.demoAdapter({products,nodes,outcome});
  const L = window.EdgezI18n;
  const t = (key,params) => L.t(S.lang,key,params);
  const a = (key,params) => t(window.EdgezAlignmentKeys[key],params);
  const c = (key,params) => t(window.EdgezConsumerKeys[key],params);
  const U={"account":"m418","notifications":"m419","general":"m420","localProfile":"m421","profileHint":"m422","chooseDevice":"m423","frequency":"m424","applications":"m425","notApplied":"m426","restoreProfile":"m427","nickname":"m428","unnamed":"m429","messages":"m430","fences":"m431","sound":"m432","vibration":"m433","notificationHint":"m434","units":"m435","metric":"m436","imperial":"m437","storage":"m438","mediaCache":"m439","clearCache":"m440","clearHint":"m441","mediaCleared":"m442","nearbyDevice":"m443","identifying":"m444","unknownDevice":"m445","identifyFailed":"m446","incompatible":"m447","saveProfile":"m448","applyProfile":"m449","savedProfile":"m450","deviceSettings":"m451","driverHint":"m452","notificationTest":"m453","keepteenIntro":"m454","edgezIntro":"m455","website":"m456","areaAll":"m457","areaRanch":"m458","areaGarden":"m459","areaWarehouse":"m460","areas":"m461","areaEmpty":"m462"};
  const u = (key,params) => t(U[key],params);
  const WEB_VERSION = '2026.11.14';
  const brandLogo = () => `<img src="assets/brand/keepteen-logo-${document.body.classList.contains('dark-mode')?'dark':'light'}@3x.png" alt="KEEPTEEN" width="156" height="59">`;
  const serialName = n => n.serial.replace(/^DEMO-/, '');
  const details = rows => `<dl class="pz-details">${rows.map(([label,value])=>`<div><dt>${label}</dt><dd>${esc(value)}</dd></div>`).join('')}</dl>`;
  const meshLabel = p => p.connections.mesh==='active'?t('m009'):p.connections.mesh==='listening'?(p.type==='chat'?t('m122'):c('meshWaiting')):t('m010');
  const cloudLabel = p => p.connections.cloud==='online'?t('m009'):S.account?t('m010'):c('cloudOff');
  const identityName = () => S.identity.name || u('unnamed');
  const peerName = p => t(({p1:'m255',p2:'m256',p3:'m257'})[p.id]);
  const messageText = m => m.textKey ? t(m.textKey) : m.text || (m.attachmentKind?t(m.attachmentKind==='image'?'m281':'m282'):'');
  const nodeName = n => n.alias || (/^\d+$/.test(n.name) ? t('m026')+' '+n.name : n.name);
  const fenceName = f => f.nameKey ? t(f.nameKey) : f.name;
  const I = (name,extra='') => `<svg class="pz-icon ${extra}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${({
    image:'<rect x="3" y="3" width="18" height="18" rx="4"/><circle cx="8" cy="8" r="1.5"/><path d="m4 17 5-5 4 4 3-3 5 5"/>',mic:'<rect x="8" y="2" width="8" height="13" rx="4"/><path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3M8 22h8"/>',
    back:'<path d="m15 5-7 7 7 7"/>',next:'<path d="m9 5 7 7-7 7"/>',plus:'<path d="M12 5v14M5 12h14"/>',close:'<path d="m6 6 12 12M6 18 18 6"/>',
    home:'<rect x="3" y="3" width="7" height="7" rx="2"/><rect x="14" y="3" width="7" height="7" rx="2"/><rect x="3" y="14" width="7" height="7" rx="2"/><rect x="14" y="14" width="7" height="7" rx="2"/>',
    user:'<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>',radio:'<path d="M5 4h14a3 3 0 0 1 3 3v9a3 3 0 0 1-3 3h-9l-5 3 1-3H5a3 3 0 0 1-3-3V7a3 3 0 0 1 3-3Z"/><path d="M7 9h10M7 13h7"/>',
    gateway:'<rect x="3" y="12" width="18" height="8" rx="3"/><path d="M6 12V6M18 12V6M9 5a5 5 0 0 1 6 0M10 8a3 3 0 0 1 4 0M7 16h.01M11 16h.01"/>',
    map:'<path d="m3 5 6-2 6 2 6-2v16l-6 2-6-2-6 2V5ZM9 3v16M15 5v16"/>',pin:'<path d="M19 10c0 5-7 11-7 11S5 15 5 10a7 7 0 0 1 14 0Z"/><circle cx="12" cy="10" r="2"/>',
    collar:'<ellipse cx="12" cy="12" rx="10" ry="6"/><rect x="8" y="8" width="8" height="8" rx="2"/>',
    fence:'<path d="M3 9V4h5M16 4h5v5M21 16v5h-5M8 21H3v-5"/><path d="M8 12h8M12 8v8"/>',settings:'<path d="M4 7h16M4 17h16"/><circle cx="9" cy="7" r="3" fill="var(--bg-card)"/><circle cx="15" cy="17" r="3" fill="var(--bg-card)"/>',
    send:'<path d="m4 12 16-8-6 17-3-7-7-2ZM11 14l9-10"/>',chat:'<path d="M21 11a9 9 0 0 1-9 9H4l-2 2V11a9 9 0 0 1 19 0Z"/><path d="M7 10h10M7 14h6"/>',
    shield:'<path d="m12 2 9 4v6c0 5-9 10-9 10S3 17 3 12V6Z"/><path d="m8 12 3 3 5-6"/>',bluetooth:'<path d="m7 7 10 10-5 4V3l5 4L7 17"/>',battery:'<rect x="2" y="6" width="18" height="12" rx="3"/><path d="M23 10v4M6 10h6v4H6z"/>',
    halow:'<path d="M4 8v9M10 8v9M4 12.5h6M14 9a5 5 0 0 1 0 7M17 5a10.5 10.5 0 0 1 0 15"/>',
    bell:'<path d="M5 17h14l-2-4V8A5 5 0 0 0 7 8v5l-2 4ZM10 21h4"/>',info:'<circle cx="12" cy="12" r="10"/><path d="M12 11v6M12 7h.01"/>',
    valve:'<path d="M2 13h20v7H2zM8 13V8h8v5M12 8V4M6 4h12"/>',camera:'<rect x="2" y="5" width="16" height="13" rx="4"/><circle cx="10" cy="11.5" r="3.5"/><path d="m18 9 4-3v12l-4-3M8 18v4M5 22h6"/>',
    minus:'<path d="M5 12h14"/>',refresh:'<path d="M20 7v5h-5M4 17v-5h5M20 12a8 8 0 0 0-14-5M4 12a8 8 0 0 0 14 5"/>',
  })[name]||''}</svg>`;
  const action = (act,label,cls='pz-primary',data='') => `<button type="button" class="${cls}" data-act="${act}" ${data}>${label}</button>`;
  const targetName = target => {const p=productByKey(target);return p?(p.alias||t(p.nameKey)+(p.key==='gateway2'?' 02':'')):t('m002');};
  const gatewayNodes = (key=S.activeGateway) => {const p=productByKey(key);return p.cache.known&&p.cache.status!=='denied'&&p.permission!=='denied'?nodes.filter(n=>n.gatewayId===p.id):[];};
  const liveNodes = () => gatewayNodes().slice(0,currentGateway().cache.limit);
  const hasLink = (target=activeTarget()) => M.ready(productByKey(target));
  const hasDataPath = (target=activeTarget()) => M.accessible(productByKey(target));
  const age = time => {
    if(!time) return t('m002');
    const sec=Math.max(0,Math.floor((Date.now()-time)/1000));
    return sec<60?t('m003',{p0:sec}):sec<3600?t('m004',{p0:Math.floor(sec/60)}):t('m005',{p0:Math.floor(sec/3600)});
  };
  const recent = n => M.freshness(n)==='fresh';
  const nodeStatus = n => !hasDataPath()?a('cachedData'):M.freshness(n)==='unknown'?t('m029')+' '+age(n.lastSeenAt):recent(n)?t('m007'):t('m008');
  const linkLabel = target => hasLink(target)?t('m009'):t('m010');
  const configStatus = item => ({unknown:a('unreadConfig'),pending:t('m147'),cached:a('cached'),transmitting:a('transmitting'),applied:t('m146'),failed:t('m149'),expired:a('expired')})[item.status]||t('m002');
  const matchingNodes = () => gatewayNodes().filter(n=>(nodeName(n)+' '+n.serial).toLowerCase().includes(S.search.toLowerCase())&&(S.filter==='all'||S.filter==='stale'&&!recent(n)||S.filter==='no-fix'&&n.latitude===null));
  const filteredNodes = () => matchingNodes().slice(0,currentGateway().cache.limit);
  async function loadNodes(key=S.activeGateway,more=false){
    const p=productByKey(key),token=++p.cache.request,previous=p.cache.known;
    p.cache.status='loading';p.cache.error=null;render();
    try{const result=await adapter.listNodes(key);if(token!==p.cache.request)return false;p.cache.known=true;p.cache.status='ready';if(hasLink(key))p.connections.mesh=result.rows.length?'active':'listening';if(more)p.cache.limit+=4;persist();return true;}
    catch(error){if(token!==p.cache.request)return false;p.cache.status=error.message==='denied'?'denied':previous?'partial':'failed';p.cache.error=error.message;return false;}
    finally{if(token===p.cache.request&&S.route==='collar'&&S.activeGateway===key)render();}
  }
  function persist() {
    try {localStorage.setItem(STORE,JSON.stringify({schemaVersion:2,extraGateway:productByKey('gateway2').present,productNames:Object.fromEntries(products.map(p=>[p.id,p.alias])),knownGateways:products.filter(p=>p.cache.known).map(p=>p.id),nodeSamples:Object.fromEntries(nodes.map(n=>[n.id,{name:n.name,lastSeenAt:n.lastSeenAt,observations:n.observations}])),packages:S.packages.filter(p=>p.status==='ready'),language:S.lang,appearance:S.appearance,fences:S.fences,identityName:S.identity.name,networkProfile:S.networkProfile,preferences:S.preferences,radio:S.radio,nodeNames:Object.fromEntries(nodes.map(n=>[n.id,n.alias]))}));S.storageError=false;}
    catch {S.storageError=true;toast(t('m011'));}
  }
  let toastTimer;
  function toast(message) { const el=$('toast');el.textContent=message;el.classList.add('show');clearTimeout(toastTimer);toastTimer=setTimeout(()=>el.classList.remove('show'),2400); }
  const media=window.EdgezMedia.create({language:()=>S.lang,notify:toast,onChange:type=>{
    if(type==='tick'){const el=$('app').querySelector('[data-record-time]');if(el)el.textContent=L.number(S.lang,media.state.seconds)+' s';}
    else if(['voice','compose-image'].includes(S.modal))render();
  }});
  function navigate(route) { if(['account','reset-password'].includes(S.route)&&route!==S.route){S.authEpoch=(S.authEpoch||0)+1;S.authBusy=false;S.accountPassword='';S.accountCode='';}media.discard();if(S.route==='collar')S.views[S.displayedGateway||S.activeGateway]={search:S.search,filter:S.filter,tab:S.collarTab};if(route==='collar'&&S.route!=='node'){const view=S.views[S.activeGateway]||{};S.search=view.search||'';S.filter=view.filter||'all';S.collarTab=view.tab||'nodes';}if(route==='collar'||route==='node')S.displayedGateway=S.activeGateway;S.route=route;S.modal=null;S.error='';S.resetScroll=true;render(); }
  const head = (title,back='home',right='') => `<header class="pz-head">${action('nav',I('back')+t('m012'),'pz-back',`data-route="${back}"`)}<h2>${esc(title)}</h2>${right||'<span style="width:44px"></span>'}</header>`;
  const empty = (icon,title,desc,button='') => `<div class="pz-card pz-empty">${I(icon)}<div class="pz-title">${title}</div><p class="pz-sub">${desc}</p>${button}</div>`;
  const tabs = (items,current,act) => `<div class="pz-segments" role="tablist">${items.map(([id,label])=>`<button role="tab" aria-selected="${current===id}" data-act="${act}" data-value="${id}">${label}</button>`).join('')}</div>`;
  function connection(target){
    const p=productByKey(target),connected=hasLink(target);
    const summary=connected?(p.type==='collar'&&p.connections.mesh==='listening'?c('meshWaiting'):t('m009')):p.connections.cloud==='online'?c('onlinePath'):t('m014');
    return `<button class="pz-card pz-connection" data-act="connect" data-target="${target}"><span class="pz-symbol">${I(p.type==='chat'?'radio':'gateway')}</span><span class="pz-grow"><span class="pz-title">${esc(p.alias||(p.type==='chat'?t('m001'):t('m000')))}</span><small class="pz-connection-summary"><span class="pz-dot ${connected?'pz-online':'pz-offline'}"></span>${summary}</small></span>${I('next')}</button>`;
  }
  const HOME_AREAS=[['all','areaAll'],['ranch','areaRanch'],['garden','areaGarden'],['warehouse','areaWarehouse']];
  function home(){
    const entries=products.filter(p=>p.present).map(p=>({id:p.key,name:targetName(p.key),icon:p.type==='chat'?'radio':'collar',status:p.type==='chat'?linkLabel(p.key):p.cache.known?(hasDataPath(p.key)?L.number(S.lang,gatewayNodes(p.key).length)+' '+t('m035'):a('cachedData')):hasLink(p.key)?t('m044'):t('m010'),online:hasDataPath(p.key)}));
    entries.push({id:'valve',name:t('m016'),icon:'valve',status:t('m017')},{id:'camera',name:t('m018'),icon:'camera',status:t('m017')});
    const areas=`<div class="pz-area-tabs" role="tablist" aria-label="${u('areas')}">${HOME_AREAS.map(([id,key])=>`<button type="button" id="area-tab-${id}" role="tab" aria-selected="${S.homeArea===id}" aria-controls="area-panel" tabindex="${S.homeArea===id?'0':'-1'}" data-act="home-area" data-value="${id}">${u(key)}</button>`).join('')}</div>`;
    const content=S.homeArea==='all'?`<div class="pz-product-grid">${entries.map(p=>`<button class="pz-product" data-act="product" data-value="${p.id}"><span class="pz-product-visual pz-neutral">${window.EdgezProductArt[p.icon]}</span><span class="pz-title">${esc(p.name)}</span><span class="pz-small"><span><span class="pz-dot ${p.online?'pz-online':'pz-offline'}"></span>${p.status}</span>${I('next')}</span></button>`).join('')}</div>${action('add',I('plus')+t('m023'),'pz-add')}`:`<div class="pz-area-empty">${I('pin')}<p>${u('areaEmpty')}</p></div>`;
    return `<div class="pz-page pz-home"><header class="pz-head pz-home-head"><h1>${t('m019')}<span class="pz-title-mark" aria-hidden="true">.</span></h1>${action('add',I('plus'),'pz-icon-btn pz-home-add',`aria-label="${t('m020')}"`)}</header>${areas}<section id="area-panel" role="tabpanel" aria-labelledby="area-tab-${S.homeArea}">${content}</section></div>`;
  }
  function nodeCard(n){
    const voltage=n.capabilities.batteryVoltage!==false?`<span><b>${n.batteryVoltage===null?'—':L.number(S.lang,n.batteryVoltage,{minimumFractionDigits:2,maximumFractionDigits:2})+' V'}</b>${t('m027')}</span>`:'';
    return `<button class="pz-node" data-act="node" data-id="${n.id}"><span class="pz-node-top"><span class="pz-avatar">${I('collar')}</span><span class="pz-grow"><span class="pz-title">${esc(nodeName(n))}</span>${n.alias||n.name!==n.hardwareName?`<br><span class="pz-small">${c('deviceSerial')} · ${esc(serialName(n))}</span>`:''}</span>${I('next')}</span><span class="pz-node-metrics">${voltage}<span><b>${n.rssi===null?'—':n.rssi+' dBm'}</b>${t('m028')}</span><span><b>${age(n.lastSeenAt)}</b>${t('m029')}</span></span><span class="pz-small" style="display:block;margin-top:10px">${nodeStatus(n)} · ${n.latitude===null?t('m030'):M.positionFresh(n)?t('m031'):t('m032')}</span></button>`;
  }
  function collar(){
    const p=currentGateway(),list=gatewayNodes();
    return `<div class="pz-page">${head(targetName(S.activeGateway),'home',action('settings',I('settings'),'pz-icon-btn',`data-target="${S.activeGateway}" aria-label="${t('m187')}"`))}${connection(S.activeGateway)}${p.cache.status==='denied'?empty('shield',t('m323'),'',action('reload-nodes',a('retry'),'pz-secondary')):`<div class="pz-stat-grid"><div class="pz-stat"><b>${p.cache.known?list.length:'—'}</b><span class="pz-small">${t('m033')}</span></div><div class="pz-stat"><b>${p.cache.known?list.filter(recent).length:'—'}</b><span class="pz-small">${t('m007')}</span></div><div class="pz-stat"><b>${p.cache.known?list.filter(n=>n.latitude===null).length:'—'}</b><span class="pz-small">${t('m030')}</span></div></div>${!hasDataPath()&&p.cache.known?`<p class="pz-notice">${a('cachedData')} · ${age(Math.max(...list.map(n=>n.lastSeenAt)))}</p>`:''}${tabs([['nodes',t('m035')],['map',t('m036')],['fences',t('m037')]],S.collarTab,'collar-tab')}${S.collarTab==='nodes'?`<div class="pz-tools"><input id="node-search" type="search" placeholder="${t('m038')}" aria-label="${t('m039')}" value="${esc(S.search)}"><select id="node-filter" aria-label="${t('m040')}">${[['all',t('m041')],['stale',t('m042')],['no-fix',t('m030')]].map(([v,label])=>`<option value="${v}" ${S.filter===v?'selected':''}>${label}</option>`).join('')}</select></div><div id="node-results">${nodeResults(filteredNodes(),list)}</div>`:S.collarTab==='map'?mapPanel():fencePanel()}`}</div>`;
  }
  function nodeResults(filtered,list){
    const p=currentGateway(),c=p.cache;
    const notice=c.status==='loading'?`<p class="pz-notice" role="status">${a('loading')}</p>`:['partial','failed'].includes(c.status)?`<div class="pz-notice">${a(c.status==='partial'?'partial':'loadFailed')}${action('reload-nodes',a('retry'),'pz-text-btn')}</div>`:'';
    return notice+(filtered.length?filtered.map(nodeCard).join(''):empty('collar',list.length?t('m043'):t('m044'),list.length?t('m045'):hasLink()?t('m046'):t('m047'),list.length?'':!hasLink()?action('connect',t('m048'),'pz-primary',`data-target="${S.activeGateway}"`):action('reload-nodes',t('m039'),'pz-secondary',c.status==='loading'?'disabled':'')))+(matchingNodes().length>filtered.length?action('more-nodes',a('loadMore'),'pz-secondary',c.status==='loading'?'disabled':''):list.length?`<p class="pz-small">${a('allLoaded')}</p>`:'');
  }
  function nodeDetail(){
    const n=gatewayNodes().find(n=>n.id===S.nodeId);if(!n)return empty('collar',t('m049'),'');
    const position=n.observations.position,signal=n.observations.signal;
    const extra=[[c('deviceSerial'),serialName(n)],[t('m051'),targetName(S.activeGateway)]];
    if(n.expectedIntervalMs)extra.push([a('cadence'),L.number(S.lang,n.expectedIntervalMs/1000)+' s']);
    if(n.latitude!==null)extra.push([t('m054'),`${n.latitude.toFixed(6)}, ${n.longitude.toFixed(6)}`]);
    return `<div class="pz-page">${head(nodeName(n),'collar')}<div class="pz-card"><div class="pz-title">${nodeStatus(n)}</div><p class="pz-sub">${t('m029')} · ${age(n.lastSeenAt)}</p></div><div class="pz-pairs">${n.capabilities.batteryVoltage!==false?`<div class="pz-card"><div class="pz-small">${t('m027')}</div><div class="pz-value">${n.batteryVoltage===null?'—':L.number(S.lang,n.batteryVoltage,{maximumFractionDigits:2})+' V'}</div><div class="pz-small">${n.batteryVoltage===null?t('m002'):a('cachedData')+' · '+age(n.observations.battery.receivedAt)}</div></div>`:''}<div class="pz-card"><div class="pz-small">${t('m028')}</div><div class="pz-value">${n.rssi??'—'}<small>${n.rssi===null?'':' dBm'}</small></div><div class="pz-small">${age(signal.sampleAt)}</div></div></div><div class="pz-card"><div class="pz-title">${t('m054')}</div><p class="pz-sub">${n.latitude===null?t('m055'):M.positionFresh(n)?t('m031'):t('m032')}</p>${n.locationAt?`<div class="pz-small">${t('m056')+age(n.locationAt)}</div>`:''}</div>${n.latitude!==null?action('locate-node',I('map')+t('m058'),'pz-primary',`data-id="${n.id}"`):''}${action('rename-node',t('m059'),'pz-secondary',`data-id="${n.id}"`)}<details class="pz-advanced pz-card"><summary>${c('moreInfo')}</summary>${details(extra)}</details></div>`;
  }
  const pointXY = p => ({x:(p.longitude-121.465)/.020*360,y:(31.241-p.latitude)/.020*300});
  const pointGeo = (x,y) => ({latitude:31.241-y/300*.020,longitude:121.465+x/360*.020});
  function contains(f,p) {
    if(!p||!Number.isFinite(p.latitude)||!Number.isFinite(p.longitude))return null;
    const lat=(p.latitude-f.center.latitude)*111320,lon=(p.longitude-f.center.longitude)*111320*Math.cos(f.center.latitude*Math.PI/180),a=-(f.rotationDegrees||0)*Math.PI/180;
    const x=lon*Math.cos(a)-lat*Math.sin(a),y=lon*Math.sin(a)+lat*Math.cos(a);
    if(f.shape==='circle')return x*x+y*y<=f.radiusMeters**2;
    if(f.shape==='oval')return (x/f.radiusXMeters)**2+(y/f.radiusYMeters)**2<=1;
    if(f.shape==='rectangle')return Math.abs(x)<=f.widthMeters/2&&Math.abs(y)<=f.heightMeters/2;
    let inside=false;for(let i=0,j=f.vertices.length-1;i<f.vertices.length;j=i++) {const a=f.vertices[i],b=f.vertices[j];if((a.latitude>p.latitude)!==(b.latitude>p.latitude)&&p.longitude<(b.longitude-a.longitude)*(p.latitude-a.latitude)/(b.latitude-a.latitude)+a.longitude)inside=!inside;}return inside;
  }
  function fenceSvg(f,draft=false) {
    if(!Number.isFinite(f.center?.latitude)||!Number.isFinite(f.center?.longitude)||!Number.isFinite(f.rotationDegrees))return '';
    const dimensions={circle:['radiusMeters'],oval:['radiusXMeters','radiusYMeters'],rectangle:['widthMeters','heightMeters'],polygon:[]}[f.shape]||[];
    if(dimensions.some(key=>!Number.isFinite(f[key])||f[key]<=0))return '';
    const c=pointXY(f.center),sx=360/(.020*111320*Math.cos(f.center.latitude*Math.PI/180)),sy=300/(.020*111320);
    const attrs=`fill="var(--map-fence-fill)" stroke="var(--map-fence)" stroke-width="1.7" ${draft?'stroke-dasharray="5 4"':''}`;
    if(f.shape==='polygon')return `<polygon points="${f.vertices.map(p=>{const q=pointXY(p);return q.x+','+q.y;}).join(' ')}" ${attrs}/>${f.vertices.map(p=>{const q=pointXY(p);return `<circle cx="${q.x}" cy="${q.y}" r="3" fill="var(--map-fence)"/>`;}).join('')}`;
    const geometry=f.shape==='rectangle'?`<rect x="${-f.widthMeters*sx/2}" y="${-f.heightMeters*sy/2}" width="${f.widthMeters*sx}" height="${f.heightMeters*sy}" rx="2" ${attrs}/>`:`<ellipse rx="${(f.shape==='circle'?f.radiusMeters:f.radiusXMeters)*sx}" ry="${(f.shape==='circle'?f.radiusMeters:f.radiusYMeters)*sy}" ${attrs}/>`;
    return `<g transform="translate(${c.x} ${c.y}) rotate(${-(f.rotationDegrees||0)})">${geometry}</g>`;
  }
  function mapSvg(editor=false) {
    const m=S.map;return `<div class="pz-map"><svg id="${editor?'pz-map-editor':'pz-map'}" data-crs="WGS84" data-map="${editor?'editor':'view'}" viewBox="0 0 360 300" preserveAspectRatio="none" role="group" aria-label="${t('m061')}" data-editor="${editor}"><g id="${editor?'pz-map-content-editor':'pz-map-content'}" data-map-content="true" transform="translate(${m.x} ${m.y}) translate(180 150) scale(${m.zoom}) translate(-180 -150)"><rect x="-2000" y="-2000" width="4400" height="4300" fill="var(--map-surface)"/><path d="M0 20 145 0 205 95 165 190 0 160Z" fill="var(--map-land)"/><path d="m235 145 125-35v190H205Z" fill="var(--map-land-alt)"/><path d="M-80 250Q70 105 190 155T450 0" fill="none" stroke="var(--map-water)" stroke-width="24"/><path d="M-40 75 400 240M90-40l75 400" stroke="var(--map-road)" stroke-width="11"/><path d="M-40 75 400 240M90-40l75 400" stroke="var(--map-road-line)" stroke-width="1" stroke-dasharray="5 4"/><text x="25" y="35" fill="var(--map-label)" font-size="11">${t('m062')}</text><text x="240" y="280" fill="var(--map-label)" font-size="11">${t('m063')}</text>${editor?fenceSvg(S.fenceDraft,true):S.fences.filter(f=>f.gatewayId===currentGateway().id&&f.enabled).map(f=>fenceSvg(f)).join('')}${gatewayNodes().filter(n=>n.latitude!==null).map(n=>{const p=pointXY(n),selected=S.selectedMapNode===n.id;return `<g class="pz-marker" role="button" tabindex="0" aria-label="${esc(nodeName(n))}" data-act="map-node" data-id="${n.id}" transform="translate(${p.x} ${p.y})"><circle r="${selected?13:10}" fill="${M.positionFresh(n)?'var(--badge-online)':'#8e8e93'}" stroke="white" stroke-width="3"/><text y="-17" text-anchor="middle" fill="var(--map-node-label)" font-size="11" font-weight="600">${esc(n.name)}</text></g>`;}).join('')}</g></svg><div class="pz-map-actions">${action('zoom',I('plus'),'pz-icon-btn',`data-value="in" aria-label="${t('m260')}"`)}${action('zoom',I('minus'),'pz-icon-btn',`data-value="out" aria-label="${t('m261')}"`)}${action('zoom',I('refresh'),'pz-icon-btn',`data-value="reset" aria-label="${t('m262')}"`)}</div><div class="pz-map-caption">${t('m064')}</div></div>`;
  }
  function mapPanel() {
    const located=gatewayNodes().filter(n=>n.latitude!==null),selected=located.find(n=>n.id===S.selectedMapNode);
    return `${mapSvg()}<div class="pz-map-key"><span class="pz-blue">● ${t('m065')}</span><span>● ${t('m066')}</span><span>${located.length} ${t('m067')}</span></div>${selected?nodeCard(selected):empty('pin',t('m069'),t('m070'))}${gatewayNodes().some(n=>n.latitude===null)?`<div class="pz-notice">${t('m071')}</div>`:''}`;
  }
  function fenceState(f){if(!f.enabled)return t('m074');return f.syncState==='monitoring'?(currentGateway().connections.cloud==='online'?a('monitoring'):a('cloudPaused')):({local:a('localOnly'),pending:a('syncPending'),syncing:a('syncing'),failed:a('syncFailed')})[f.syncState]||a('localOnly');}
  function alarms(){
    if(!S.proposal||currentGateway().connections.cloud!=='online')return [];
    return S.fences.filter(f=>f.gatewayId===currentGateway().id&&f.enabled&&f.syncState==='monitoring').flatMap(f=>gatewayNodes().filter(n=>M.positionFresh(n)&&f.deviceIds.includes(n.id)).flatMap(n=>{const inside=contains(f,n),event=inside?'enter':'exit';return (inside?f.enterAlert:f.exitAlert)?[{id:f.id+':'+f.revision+':'+n.id+':'+event+':'+n.locationAt,f,n,event}]:[];}));
  }
  function fencePanel(){
    const alerts=alarms(),fences=S.fences.filter(f=>f.gatewayId===currentGateway().id),supported=capability('fenceSync');
    const create=action('new-fence',I('plus')+t('m072'));
    if(!fences.length)return empty('fence',c('noFencesTitle'),c('noFencesBody'),create);
    const rules=fences.map(f=>`<div class="pz-card" style="margin-top:12px"><div class="pz-node-top"><span class="pz-avatar">${I('fence')}</span><div class="pz-grow"><div class="pz-title">${esc(fenceName(f))}</div><div class="pz-small">${shapeName(f.shape)}${f.shape==='circle'?' · '+distance(f.radiusMeters):''} · ${t('m076',{p0:f.deviceIds.length})}</div></div>${action('edit-fence',I('next'),'pz-icon-btn',`data-id="${f.id}" aria-label="${t('m075')}"`)}</div><p class="pz-small">${fenceState(f)}</p>${supported?action('sync-fence',a('sync'),'pz-secondary',`data-id="${f.id}" ${f.syncState==='syncing'?'disabled':''}`):''}</div>`).join('');
    if(!supported)return create+rules+`<p class="pz-notice">${c('syncUnavailable')}</p>`;
    return create+rules+`<div class="pz-section"><h3>${t('m080')}</h3><span class="pz-small">${alerts.length}</span></div>${alerts.length?alerts.map(item=>`<div class="pz-card"><div class="pz-title pz-warn">${esc(nodeName(item.n))} · ${item.event==='exit'?t('m081'):t('m082')}</div><p class="pz-small">${esc(fenceName(item.f))} · ${age(item.n.locationAt)}</p>${S.acknowledged.includes(item.id)?`<div class="pz-small">${t('m084')}</div>`:action('ack',t('m085'),'pz-secondary',`data-id="${item.id}" ${S.ackBusy?'disabled':''}`)}</div>`).join(''):empty('shield',t('m086'),fences.some(f=>f.syncState==='monitoring')?fenceState(fences.find(f=>f.syncState==='monitoring')):a('notMonitoring'))}`;
  }
  function shapeName(shape){return ({circle:t('m090'),oval:t('m091'),rectangle:t('m092'),polygon:t('m093')})[shape];}
  const channelNames = () => [t('m094'),t('m095'),t('m096'),t('m097'),t('m098')];
  function threadName(id) {return id.startsWith('c')?'#'+id.slice(1)+' '+channelNames()[Number(id.slice(1))-1]:(peers.find(p=>p.id===id)?peerName(peers.find(p=>p.id===id)):id);}
  const canSend = () => {const mesh=productByKey(S.activeChat).connections.mesh;return hasLink(S.activeChat)&&(S.thread?.startsWith('c')?['active','listening'].includes(mesh):mesh==='active'&&peers.find(p=>p.id===S.thread)?.online);};
  const messageLabel = m => ({sending:t('m099'),sent:t('m100'),delivered:t('m101'),read:t('m280'),failed:t('m102'),retrying:t('m103',{p0:m.retryCount})})[m.status]||'';
  function chat() {
    if(S.thread) {
      const history=S.messages[S.thread]||[],publicChannel=S.thread.startsWith('c');
      return `<div class="pz-page pz-conversation">${head(threadName(S.thread),'chat-list',action('thread-info',I('info'),'pz-icon-btn',`aria-label="${t('m104')}"`))}${!canSend()?`<div class="pz-notice">${hasLink()?t('m107'):t('m108')}</div>`:''}<div class="pz-messages" id="pz-messages">${history.length?history.map(messageBubble).join(''):`<div class="pz-empty pz-small">${t('m110')}</div>`}</div><div class="pz-phrases">${[t('m111'),t('m112'),t('m113')].map(text=>action('phrase',esc(text),'',`data-text="${esc(text)}" ${canSend()?'':'disabled'}`)).join('')}</div><form class="pz-composer" id="chat-form" novalidate>${action('choose-image',I('image'),'pz-icon-btn',`aria-label="${t('m283')}" ${canSend()?'':'disabled'}`)}<input name="message" id="message-input" value="${esc(S.composerDrafts[S.thread]||'')}" maxlength="240" autocomplete="off" placeholder="${t('m114')}" aria-label="${t('m115')}" ${canSend()?'':'disabled'} required>${action('voice',I('mic'),'pz-icon-btn',`aria-label="${t('m284')}" ${canSend()&&capability('voiceMessage')?'':'disabled'}`)}<button class="pz-send" type="submit" aria-label="${t('m116')}" ${canSend()?'':'disabled'}>${I('send')}</button></form><input id="image-picker" type="file" accept="image/jpeg,image/png,image/webp,image/gif" hidden><input id="audio-picker" type="file" accept="audio/*" hidden></div>`;
    }
    const list=S.chatTab==='channels'?channelNames().map((name,i)=>({id:'c'+(i+1),name:'#'+(i+1)+' '+name,icon:'chat',sub:t('m117')})):productByKey(S.activeChat).connections.mesh==='active'?peers.map(p=>({id:p.id,name:peerName(p),icon:'user',sub:!hasLink('radio')?t('m006'):p.online?t('m118'):t('m119')})):[];
    return `<div class="pz-page">${head(t('m001'),'home',action('settings',I('settings'),'pz-icon-btn',`data-target="${S.activeChat}" aria-label="${t('m187')}"`))}${connection(S.activeChat)}<div class="pz-section"><h3>${t('m120')}</h3><span class="pz-small">${productByKey(S.activeChat).connections.mesh==='active'&&hasLink(S.activeChat)?t('m121'):t('m122')}</span></div>${tabs([['channels',t('m123')],['people',t('m124')]],S.chatTab,'chat-tab')}<div class="pz-card">${list.length?list.map(item=>{const messages=S.messages[item.id]||[],last=messages[messages.length-1];return `<button class="pz-menu" data-act="thread" data-id="${item.id}"><span class="pz-avatar">${I(item.icon)}</span><span class="pz-grow"><span class="pz-title">${esc(item.name)}</span><small>${last?esc(messageText(last)):item.sub}</small></span>${I('next')}</button>`;}).join(''):empty('user',t('m125'),t('m126'))}</div><p class="pz-small">${t('m127')}</p></div>`;
  }
  function seedConversation(id) {
    if(!S.messages[id])S.messages[id]=S.lifecycle===3?[{id:'seed-'+id,mine:false,textKey:'m258',text:'Near the west fence. Returning to the rally point.',timestampMs:baseTime-120000,status:'received'}]:[];
  }
  const menu = (act,icon,title,sub,data='') => `<button class="pz-menu" data-act="${act}" ${data}>${I(icon)}<span class="pz-grow"><span class="pz-title">${title}</span>${sub?`<small>${sub}</small>`:''}</span>${I('next')}</button>`;
  function profile() {
    return `<div class="pz-page pz-settings"><header class="pz-head"><h1>${t('m128')}</h1></header><div class="pz-card pz-menu-group">${menu('account','user',u('account'),S.account?esc(S.account.email):'')}${menu('networks','halow',t('m133'),'')}</div><div class="pz-card pz-menu-group">${menu('notifications','bell',u('notifications'),'')}${menu('general','settings',u('general'),'')}</div><div class="pz-card pz-menu-group">${menu('help','shield',c('help'),'')}${menu('about','info',t('m142'),'')}</div></div>`;
  }
  function general(){return `<div class="pz-page">${head(u('general'),'profile')}<div class="pz-card pz-menu-group">${menu('language','chat',t('m138'),L.name(S.lang))}${menu('appearance','settings',t('m139'),S.appearance==='system'?c('systemAppearance'):document.body.classList.contains('dark-mode')?t('m140'):t('m141'))}${menu('units','settings',u('units'),u(S.preferences.units))}</div><div class="pz-card pz-menu-group">${menu('storage','map',t('m136'),'')}${menu('cache','image',u('storage'),'')}</div></div>`;}
  const notificationPolicy = kind => ({enabled:['messages','fences'].includes(kind)&&S.preferences.notifications[kind],sound:S.preferences.notifications.sound,vibration:S.preferences.notifications.vibration});
  function notifyEvent(kind,text,{silent=false}={}){
    const policy=notificationPolicy(kind);if(!policy.enabled)return null;
    const event={kind,text,sound:policy.sound&&!silent,vibration:policy.vibration&&!silent,at:Date.now()};S.notificationEvents.push(event);if(S.notificationEvents.length>50)S.notificationEvents.shift();
    if(!silent)toast(text);
    // Native integration consumes the same policy, then checks OS permissions separately.
    if(typeof window.CustomEvent==='function')window.dispatchEvent?.(new CustomEvent('keepteen:notification',{detail:event}));
    return event;
  }
  function receiveMessage(thread,message){
    if(!thread||!message?.id)return false;const list=S.messages[thread]||(S.messages[thread]=[]);if(list.some(m=>m.id===message.id))return false;
    const m={...message,mine:false,status:'received',timestampMs:message.timestampMs||Date.now()};list.push(m);
    if(S.route==='chat'&&S.thread===thread&&!S.modal)render();else notifyEvent('messages',messageText(m));return true;
  }
  function notifications(){return `<div class="pz-page">${head(u('notifications'),'profile')}<div class="pz-card pz-menu-group">${['messages','fences','sound','vibration'].map(k=>`<label class="pz-menu pz-switch-row"><span class="pz-grow pz-title">${u(k)}</span><input type="checkbox" role="switch" data-preference="${k}" ${S.preferences.notifications[k]?'checked':''}></label>`).join('')}</div><p class="pz-small pz-footer-note">${u('notificationHint')}</p></div>`;}
  const distance = meters => L.number(S.lang,S.preferences.units==='imperial'?meters/0.3048:meters,{style:'unit',unit:S.preferences.units==='imperial'?'foot':'meter',maximumFractionDigits:0});
  const mediaFiles = () => [...new Set(Object.values(S.messages).flat().filter(m=>m.objectUrl).map(m=>m.objectUrl))];
  function clearMediaCache(){for(const url of mediaFiles())URL.revokeObjectURL(url);for(const m of Object.values(S.messages).flat())if(m.objectUrl){delete m.objectUrl;m.cacheCleared=true;}media.discard();S.modal=null;render();}
  function deviceSettings(){const p=productByKey(S.target);return `<div class="pz-page">${head(u('deviceSettings'),p.type==='chat'?'chat':'collar')}<div class="pz-card pz-menu-group">${p.type==='chat'?menu('identity','user',u('nickname'),S.identity.name?esc(S.identity.name):u('unnamed')):''}${menu('device-network','halow',t('m133'),'')}${menu('rename-product','settings',a('renameProduct'),'')}</div>${deviceInformation()}<div class="pz-card pz-menu-group">${menu('inspect-device','info',c('diagnostics'),'',`data-target="${p.key}"`)}</div></div>`;}

  function aboutContent(){
    const theme=document.body.classList.contains('dark-mode')?'dark':'light';
    return `<div class="pz-joint-brand" aria-label="KEEPTEEN × EdgeZ.ai"><span>${brandLogo()}</span><span class="pz-brand-cross" aria-hidden="true">×</span><img src="assets/brand/edgez-logo-${theme}.svg" alt="EdgeZ.ai" width="144" height="43"></div><div class="pz-card pz-company"><h3>KEEPTEEN</h3><p class="pz-sub">${u('keepteenIntro')}</p><a class="pz-company-link" href="https://www.keepteen.com/" target="_blank" rel="noopener noreferrer" aria-label="KEEPTEEN ${u('website')}"><span>keepteen.com</span>${I('next')}</a></div><div class="pz-card pz-company"><h3>EdgeZ.ai</h3><p class="pz-sub">${u('edgezIntro')}</p><a class="pz-company-link" href="https://www.edgez.ai/" target="_blank" rel="noopener noreferrer" aria-label="EdgeZ.ai ${u('website')}"><span>edgez.ai</span>${I('next')}</a></div><div class="pz-about-version pz-small">${c('version')} ${WEB_VERSION}</div>`;
  }
  function about(){return `<div class="pz-page">${head(t('m142'),'profile')}${aboutContent()}</div>`;}
  function help(){return `<div class="pz-page">${head(c('help'),'profile')}<div class="pz-card pz-help">${['Pair','Data','Map','Offline'].map(key=>`<details><summary>${c('help'+key+'Title')}</summary><p class="pz-sub">${c('help'+key+'Body')}</p></details>`).join('')}</div>${action('diagnostics',c('diagnostics'),'pz-secondary')}</div>`;}
  function diagnostics(){return `<div class="pz-page">${head(c('diagnostics'),'profile')}<p class="pz-sub pz-intro">${c('diagnosticIntro')}</p><div class="pz-card pz-menu-group">${products.filter(p=>p.present).map(p=>menu('inspect-device',p.type==='chat'?'radio':'gateway',esc(targetName(p.key)),linkLabel(p.key),`data-target="${p.key}"`)).join('')}</div>${action('help',c('help'),'pz-secondary')}</div>`;}
  function deviceInformation(diagnostic=false){
    const p=productByKey(S.target),rows=[];
    if(diagnostic){rows.push([c('bluetooth'),linkLabel(p.key)],[c('network'),meshLabel(p)],[a('cloud'),cloudLabel(p)]);}
    rows.push([a('firmware'),p.firmwareVersion||(hasLink(p.key)?t('m002'):c('firmwarePending'))]);
    const d=S.pendingDevice||S.bindings[p.key];if(d)rows.push([c('deviceSerial'),d.advertisement]);
    return `<div class="pz-card">${details(rows)}</div>`;
  }
  function networks(){return `<div class="pz-page">${head(t('m133'),S.networkBack)}${radioForm()}</div>`;}
  function openNetwork(back='profile'){
    S.networkBack=back;S.networkOrigin=S.target;S.radioDraft=withBandwidth(S.networkProfile?.config||{country:'',halowChannel:null,meshId:'',meshPassphrase:''});S.radioAdvanced=false;S.showPass=false;navigate('networks');
  }
  function withBandwidth(d){return {...copy(d),bandwidth:catalog[d.country]?.find(c=>c.number===Number(d.halowChannel))?.bandwidthMHz||null};}
  function saveNetwork(d){
    const error=validateRadio(d);if(error){S.error=error;return false;}
    const config=canonicalRadio(d),old=S.networkProfile;if(!old||JSON.stringify(old.config)!==JSON.stringify(config))S.networkProfile={id:'phone-network',revision:(old?.revision||0)+1,config,updatedAt:Date.now()};
    persist();if(S.storageError){S.networkProfile=old;S.error=t('m011');return false;}S.error='';return true;
  }
  function profileCompatible(p,config){
    if(!p||!p.present||p.permission!=='write'||!M.capabilities(p,S.proposal).radioWrite)return false;
    // Descriptors are an adapter fixture here. Native firmware must supply verified constraints.
    const d=p.descriptor;if(p.identification!=='identified'||!d)return false;
    return d.capabilities?.radioWrite===true&&d.radioTuples?.some(r=>r.country===config.country&&r.channel===config.halowChannel)===true;
  }
  async function applyProfile(){
    const pending=S.pendingApply,p=productByKey(S.target);if(!pending||!profileCompatible(p,pending.config)){S.error=u('incompatible');render();return false;}
    const item=S.radio[S.target];if(item.inflight)return false;item.profileId=pending.id;item.profileRevision=pending.revision;return applyRadio(copy(pending.config));
  }

  function field(name,label,value,type='text',attrs='') {return `<label class="pz-field"><span>${label}</span><input name="${name}" type="${type}" value="${esc(type==='number'&&typeof value==='number'&&!Number.isFinite(value)?'':value)}" ${attrs}></label>`;}
  function selectField(name,label,options) {return `<label class="pz-field"><span>${label}</span><select name="${name}">${options}</select></label>`;}
  function radioForm(){
    const d=S.radioDraft||withBandwidth(S.networkProfile?.config||{country:'',halowChannel:null,meshId:'',meshPassphrase:''}),rows=catalog[d.country]||[],channel=rows.find(c=>c.number===Number(d.halowChannel)),bandwidth=d.bandwidth||channel?.bandwidthMHz,bws=[...new Set(rows.map(c=>c.bandwidthMHz))];
    return `<form id="radio-form" novalidate><p class="pz-sub pz-intro">${u('profileHint')}</p><div class="pz-card pz-network-fields">${selectField('country',t('m151'),'<option value="">'+u('unnamed')+'</option>'+Object.keys(catalog).map(code=>`<option value="${code}" ${d.country===code?'selected':''}>${code} · ${esc(L.region(S.lang,code))}</option>`).join(''))}${field('meshId',t('m154'),d.meshId,'text','required maxlength="32" autocomplete="off"')}${field('meshPassphrase',t('m155'),d.meshPassphrase,S.showPass?'text':'password','maxlength="63" autocomplete="off"')}<label class="pz-check"><input id="show-pass" type="checkbox" ${S.showPass?'checked':''}>${t('m156')}</label><p class="pz-small">${t('m157')}</p></div><details class="pz-advanced pz-card" ${S.radioAdvanced?'open':''}><summary>${a('advanced')}</summary>${selectField('bandwidth',t('m152'),bws.length?bws.map(bw=>`<option value="${bw}" ${bw===Number(bandwidth)?'selected':''}>${bw} MHz</option>`).join(''):'<option value="">'+u('unnamed')+'</option>')}${selectField('halowChannel',t('m153'),rows.length?rows.filter(c=>c.bandwidthMHz===Number(bandwidth)).map(c=>`<option value="${c.number}" ${c.number===Number(d.halowChannel)?'selected':''}>CH ${c.number} · ${L.number(S.lang,c.frequencyMHz)} MHz</option>`).join(''):'<option value="">'+u('unnamed')+'</option>')}${details([[u('frequency'),channel?L.number(S.lang,channel.frequencyMHz)+' MHz':'—']])}</details><div class="pz-error" role="alert">${esc(S.error)}</div><button class="pz-primary" type="submit">${u('saveProfile')}</button>${action('request-apply',u('applyProfile'),'pz-secondary')}${S.networkProfile?action('restore-profile',u('restoreProfile'),'pz-text-btn'):''}${S.networkProfile?`<p class="pz-small" role="status">${u('savedProfile')} · ${age(S.networkProfile.updatedAt)}</p>`:''}<p class="pz-small pz-footer-note">${u('driverHint')}</p></form>${Object.values(S.radio).some(r=>r.desired||r.cachedApplied)?`<div class="pz-section"><h3>${u('applications')}</h3></div><div class="pz-card pz-menu-group">${products.filter(p=>p.present&&(S.radio[p.key].desired||S.radio[p.key].cachedApplied)).map(p=>menu('application-status',p.type==='chat'?'chat':'gateway',esc(targetName(p.key)),configStatus(S.radio[p.key]),`data-target="${p.key}"`)).join('')}</div>`:''}`;
  }
  function applicationStatus(){
    const item=S.radio[S.target],cfg=item.desired,applied=item.applied||item.cachedApplied;
    return `<p class="pz-title">${esc(targetName(S.target))}</p><div class="pz-card">${details([[t('m133'),configStatus(item)],[t('m154'),cfg?.meshId||'—'],[t('m151'),cfg?.country||'—'],[t('m153'),cfg?.halowChannel||'—']])}</div>${applied?`<details class="pz-advanced pz-card"><summary>${item.applied?t('m146'):a('cached')}</summary>${details([[t('m154'),applied.meshId],[t('m151'),applied.country],[t('m153'),applied.halowChannel]])}</details>`:''}${item.error?`<p class="pz-notice" role="alert">${item.error==='confirmation-unavailable'?a('cached'):item.error==='denied'?t('m323'):a('saveFailed')}</p>`:''}${action('read-config',S.radioReadBusy?a('loading'):a('readConfig'),'pz-secondary',item.inflight||S.radioReadBusy?'disabled':'')}${item.desired?action('retry-profile',u('applyProfile'),'pz-primary',item.inflight?'disabled':''):''}<div class="pz-error" role="alert">${esc(S.error)}</div>`;
  }

  function fenceForm() {
    const f=S.fenceDraft,shape=f.shape;
    const sizeFields=shape==='circle'?field('radiusMeters',t('m162'),f.radiusMeters,'number','min="10" max="100000" required'):shape==='oval'?field('radiusXMeters',t('m163'),f.radiusXMeters,'number','min="10" max="100000" required')+field('radiusYMeters',t('m164'),f.radiusYMeters,'number','min="10" max="100000" required'):shape==='rectangle'?field('widthMeters',t('m165'),f.widthMeters,'number','min="10" max="100000" required')+field('heightMeters',t('m166'),f.heightMeters,'number','min="10" max="100000" required'):'';
    return `<form id="fence-form" novalidate>${field('name',t('m167'),f.name,'text','required maxlength="128"')}${selectField('shape',t('m168'),['circle','oval','rectangle','polygon'].map(s=>`<option value="${s}" ${shape===s?'selected':''}>${shapeName(s)}</option>`).join(''))}${mapSvg(true)}<div class="pz-small">${shape==='polygon'?t('m169'):t('m170')}</div>${shape==='polygon'?action('undo-vertex',t('m171',{p0:f.vertices.length}),'pz-text-btn'):''}${sizeFields}<details class="pz-advanced pz-card" data-fence-advanced ${S.fenceAdvanced?'open':''}><summary>${c('moreInfo')}</summary><div class="pz-pairs">${field('latitude',t('m172'),(Number.isFinite(f.center.latitude)?f.center.latitude.toFixed(6):''),'number','min="-90" max="90" step="any" required')}${field('longitude',t('m173'),(Number.isFinite(f.center.longitude)?f.center.longitude.toFixed(6):''),'number','min="-180" max="180" step="any" required')}</div>${['oval','rectangle'].includes(shape)?field('rotationDegrees',t('m174'),f.rotationDegrees,'number','min="-180" max="180" required'):''}</details>${selectField('scope',t('m175'),`<option value="all" ${S.fenceScope==='all'?'selected':''}>${a('scopeExisting')}</option><option value="selected" ${S.fenceScope==='selected'?'selected':''}>${t('m176')}</option>`)}${S.fenceScope==='selected'?`<div class="pz-card">${gatewayNodes().map(n=>`<label class="pz-check"><input type="checkbox" name="deviceIds" value="${n.id}" ${f.deviceIds.includes(n.id)?'checked':''}>${esc(nodeName(n))} · ${esc(n.serial)}</label>`).join('')}</div>`:''}<label class="pz-check"><input name="enterAlert" type="checkbox" ${f.enterAlert?'checked':''}>${t('m177')}</label><label class="pz-check"><input name="exitAlert" type="checkbox" ${f.exitAlert?'checked':''}>${t('m178')}</label><label class="pz-check"><input name="enabled" type="checkbox" ${f.enabled?'checked':''}>${t('m179')}</label><div class="pz-error" role="alert">${esc(S.error)}</div><button class="pz-primary" type="submit">${t('m180')}</button><p class="pz-small">${a('localOnly')}</p>${S.fences.some(item=>item.id===f.id)?action('delete-fence',t('m181'),'pz-secondary',`data-id="${f.id}"`):''}</form>`;
  }
  function receiptText(m){return new Date(m.timestampMs).toLocaleTimeString(L.locale(S.lang),{hour:'2-digit',minute:'2-digit'})+(m.mine?' · '+messageLabel(m):'');}
  function retryButton(m){return m.status==='failed'?action('retry',t('m109'),'pz-text-btn',`data-id="${m.id}" ${canSend()?'':'disabled'}`):'';}
  function messageBubble(m){
    let body='';
    if(m.cacheCleared)body=`<span class="pz-small">${u('mediaCleared')}</span>`;
    else if(m.attachmentKind==='image')body=`<button type="button" class="pz-image-message" data-act="view-image" data-id="${m.id}" aria-label="${t('m307')}"><img src="${esc(m.objectUrl)}" alt="${t('m281')}"></button>`;
    else if(m.attachmentKind==='voice')body=`<div class="pz-small">${I('mic')} ${t('m282')}${m.durationMs?' · '+t('m308',{p0:Math.round(m.durationMs/1000)}):''}</div><audio controls preload="metadata" src="${esc(m.objectUrl)}" aria-label="${t('m297')}"></audio>`;
    const text=m.attachmentKind?m.text:messageText(m);
    return `<div class="pz-bubble ${m.mine?'mine':''} ${m.status==='failed'?'failed':''}" data-message="${m.id}">${body}${text?`<div>${esc(text)}</div>`:''}<div class="pz-small" data-receipt>${esc(receiptText(m))}</div><div data-retry>${retryButton(m)}</div></div>`;
  }
  function refreshMessage(m,thread){
    if(S.route!=='chat'||S.thread!==thread||S.modal)return;
    const bubble=$('app').querySelector(`[data-message="${m.id}"]`);
    if(!bubble){render();return;}
    bubble.classList.toggle('failed',m.status==='failed');
    bubble.querySelector('[data-receipt]').textContent=receiptText(m);
    bubble.querySelector('[data-retry]').innerHTML=retryButton(m);
  }
  function accountPanel(){
    if(S.account)return `<div class="pz-card"><div class="pz-title">${esc(S.account.name)}</div><p class="pz-sub">${esc(S.account.email)}</p><div class="pz-small">${t('m198')}</div></div>${action('logout',t('m199'),'pz-secondary')}`;
    return `${tabs([['code',t('m299')],['password',t('m298')]],S.loginMode,'login-mode')}${S.loginMode==='password'&&!S.proposal?`<div class="pz-notice">${c('passwordPending')}</div>${action('login-mode',t('m299'),'pz-primary','data-value="code"')}`:`<form id="account-form" novalidate>${field('email',t('m200'),S.accountEmail||S.otpEmail,'email','required autocomplete="email"')}${S.loginMode==='password'?`${field('password',t('m300'),S.accountPassword,S.showAccountPassword?'text':'password','required autocomplete="current-password"')}<label class="pz-check"><input id="show-account-password" type="checkbox" ${S.showAccountPassword?'checked':''}>${t('m301')}</label>`:S.otpEmail?field('otp',t('m201'),S.accountCode,'text','required inputmode="numeric" maxlength="6" autocomplete="one-time-code"'):''}<div class="pz-error" role="alert">${esc(S.error)}</div><button class="pz-primary" type="submit" ${S.authBusy?'disabled':''}>${S.authBusy?a('saving'):S.loginMode==='password'?t('m129'):S.otpEmail?t('m202'):t('m203')}</button><p class="pz-small">${t('m204')}</p></form>`}${S.loginMode==='password'&&S.proposal?action('reset-password',a('resetPassword'),'pz-text-btn'):''}`;
  }
  function accountPage(){return `<div class="pz-page pz-account-page">${head(u('account'),'profile')}${accountPanel()}</div>`;}
  function resetPasswordPage(){return `<div class="pz-page pz-account-page">${head(a('resetPassword'),'account')}${S.proposal?`<form id="reset-form" novalidate>${field('email',t('m200'),S.resetEmail||S.accountEmail,'email','required autocomplete="email"')}<p class="pz-error" role="alert">${esc(S.error)}</p>${S.resetSent?`<p role="status">${a('resetSent')}</p>`:''}<button class="pz-primary" type="submit" ${S.authBusy?'disabled':''}>${a('sendReset')}</button></form>`:`<p class="pz-notice">${c('passwordPending')}</p>`}</div>`;}
  function storagePanel(){return `<div class="pz-card"><div class="pz-title">${a('mapRegion')}</div><p class="pz-small">${c('mapUnavailable')}</p>${S.packages.length?S.packages.map(pkg=>`<div class="pz-row"><span class="pz-grow">${a('mapRegion')}<small class="pz-small" style="display:block">${pkg.status==='ready'?a('packageReady'):pkg.status==='failed'?a('downloadFailed'):pkg.status==='queued'?a('preparing'):a('downloading')+' '+pkg.progress+'%'}</small></span>${['queued','downloading'].includes(pkg.status)?action('cancel-map',t('m289'),'pz-text-btn',`data-id="${pkg.id}"`):pkg.status==='failed'?action('download-map',a('retry'),'pz-text-btn')+action('request-delete-map',c('delete'),'pz-text-btn',`data-id="${pkg.id}"`):action('request-delete-map',c('delete'),'pz-text-btn',`data-id="${pkg.id}"`)}</div>`).join(''):`<p class="pz-sub">${t('m210')}</p>`}${!S.packages.some(p=>['queued','downloading','ready'].includes(p.status))?action('download-map',a('download'),'pz-primary'):''}</div>`;}
  function mediaPanel(){
    const d=media.state;
    if(S.modal==='image-view'){
      const message=Object.values(S.messages).flat().find(m=>m.id===S.imageMessageId);
      return message?`<img class="pz-media-preview" src="${esc(message.objectUrl)}" alt="${t('m281')}">`:'';
    }
    if(S.modal==='compose-image')return `<img class="pz-media-preview" src="${esc(d.url)}" alt="${t('m281')}">${field('caption',t('m290'),S.caption,'text','maxlength="240"')}${action('send-image',t('m287'))}${action('close',t('m289'),'pz-secondary')}`;
    return `<div class="pz-voice-stage">${I('mic')}<div class="pz-value" data-record-time>${d.recording?L.number(S.lang,d.seconds)+' s':t('m284')}</div><div class="pz-small">${d.recording?t('m296'):t('m306')}</div></div>${d.url?`<audio class="pz-audio-preview" controls src="${esc(d.url)}" aria-label="${t('m297')}"></audio>${d.durationMs?`<p class="pz-small">${t('m308',{p0:Math.round(d.durationMs/1000)})}</p>`:''}${action('send-voice',t('m288'))}`:''}${d.recording?action('stop-recording',t('m286'),'pz-secondary'):action('start-recording',t('m285'),'pz-secondary',d.requesting?'disabled':'')}${!d.recording?action('choose-audio',t('m295'),'pz-text-btn'):''}`;
  }
  const discovered=[{id:'DEMO-GW-01',advertisement:'EZ_02000000A001',target:'gateway',rssi:-48},{id:'DEMO-CHAT-01',advertisement:'NRF_02000000B001',target:'radio',rssi:-63},{id:'DEMO-GW-02',advertisement:'EZ_02000000A002',target:'gateway2',rssi:-69}];
  let searchEpoch=0;
  async function startSearch(){
    const token=++searchEpoch;S.pendingDevice=null;S.scanStatus='scanning';openModal('add');
    const outcome=$('demo-scan')?.value||'found';
    await new Promise(resolve=>setTimeout(resolve,700));
    if(token!==searchEpoch||S.modal!=='add')return;
    S.scanStatus=['empty','off','denied'].includes(outcome)?outcome:'found';render();
  }
  function discoveryPanel(){
    if(S.scanStatus==='scanning')return `<div class="pz-empty"><div class="pz-scan-sun" aria-hidden="true">${Array.from({length:12},(_,i)=>`<i style="--ray:${i}"></i>`).join('')}<span>${I('bluetooth')}</span></div><div class="pz-title" role="status">${t('m312')}</div><p class="pz-sub">${t('m313')}</p></div>`;
    if(S.scanStatus==='off'||S.scanStatus==='denied')return empty('info',t(S.scanStatus==='off'?'m322':'m323'),t(S.scanStatus==='off'?'m315':'m316'),action('search-again',t('m317')));
    if(S.scanStatus==='empty'||S.scanStatus==='idle')return empty('bluetooth',t('m314'),t('m313'),action('search-again',t('m317')));
    return `<p class="pz-sub">${t('m325')}</p><div class="pz-card">${discovered.map(d=>`<button type="button" class="pz-menu" data-act="discovered-device" data-id="${d.id}"><span class="pz-avatar">${I('bluetooth')}</span><span class="pz-grow"><span class="pz-title">${u('nearbyDevice')}</span><small>${c('deviceSerial')} · ••${esc(d.advertisement.slice(-6))}</small></span>${I('next')}</button>`).join('')}</div>${action('search-again',t('m317'),'pz-secondary')}`;
  }
  function connectionPanel(){
    const pairNeeded=Boolean(S.pendingDevice)||!hasLink(S.target);
    return `<div class="pz-device-hero"><span class="pz-avatar">${I(productByKey(S.target).type==='chat'?'radio':'gateway')}</span><div class="pz-title">${S.pendingDevice?u('nearbyDevice'):esc(targetName(S.target))}</div><p class="pz-sub">${S.busy?u('identifying'):S.pendingDevice?u('nearbyDevice'):linkLabel(S.target)}</p></div>${pairNeeded?`<p class="pz-sub pz-intro">${t('m185')}</p>${action('pair',S.busy?t('m240'):t('m186'),'pz-primary',S.busy?'disabled':'')}`:action('settings',t('m187'),'pz-primary',`data-target="${S.target}"`)}<div class="pz-section"><h3>${c('deviceInfo')}</h3></div>${S.pendingDevice?'':deviceInformation()}<div class="pz-error" role="alert">${esc(S.error)}</div>${S.pendingDevice?'':action('rename-product',a('renameProduct'),'pz-secondary')}${S.pendingDevice?'':action('inspect-device',c('diagnostics'),'pz-text-btn',`data-target="${S.target}"`)}`;
  }
  function modal() {
    if(!S.modal)return '';
    let title='',body='';
    if(S.modal==='fence'){title=t('m182');body=fenceForm();}
    if(S.modal==='apply-status'){title=t('m133');body=radioForm();}
    if(S.modal==='connection'){title=targetName(S.target);body=connectionPanel();}
    if(S.modal==='add'){title=t('m318');body=discoveryPanel();}
    if(S.modal==='language'){title=t('m252');body=`<p class="pz-sub">${t('m253')}</p><div role="radiogroup" aria-label="${t('m252')}">${L.languages.map(item=>`<button type="button" class="pz-menu pz-language-choice" role="radio" aria-checked="${S.lang===item.code}" data-act="set-language" data-value="${item.code}"><span class="pz-grow" lang="${item.locale}">${item.name}</span><span class="pz-blue">${S.lang===item.code?'✓':''}</span></button>`).join('')}</div>`;}
    if(S.modal==='identity'){title=u('nickname');body=`<form id="identity-form" novalidate>${field('name',u('nickname'),S.identity.name,'text','required maxlength="40"')}<p class="pz-small">${c('identityHint')}</p><details class="pz-advanced pz-card"><summary>${c('moreInfo')}</summary>${details([[t('m194'),hasLink(S.activeChat)?S.identity.nodeId:c('firmwarePending')],[t('m195'),S.identity.publicKey.startsWith('DEMO-')?t('m002'):S.identity.publicKey]])}</details><div class="pz-error" role="alert">${esc(S.error)}</div><button class="pz-primary" type="submit">${t('m196')}</button></form>`;}
    if(S.modal==='appearance'){title=t('m139');body=`<div class="pz-card pz-menu-group" role="radiogroup">${[['system',c('systemAppearance')],['light',t('m141')],['dark',t('m140')]].map(([value,label])=>`<button class="pz-menu" role="radio" aria-checked="${S.appearance===value}" data-act="set-appearance" data-value="${value}"><span class="pz-grow">${label}</span><span class="pz-blue">${S.appearance===value?'✓':''}</span></button>`).join('')}</div>`;}
    if(['voice','compose-image','image-view'].includes(S.modal)){title=t(S.modal==='voice'?'m284':'m307');body=mediaPanel();}
    if(S.modal==='thread-info'){title=t('m104');body=`<div class="pz-card"><div class="pz-title">${esc(threadName(S.thread))}</div><p class="pz-sub">${S.thread.startsWith('c')?t('m205'):t('m206')}</p></div>`;}
    if(S.modal==='rename'){title=t('m207');const n=nodes.find(n=>n.id===S.nodeId);body=`${tabs([['local',a('localAlias')],['shared',a('sharedName')]],S.renameScope,'rename-scope')}`+`<form id="rename-form" novalidate>${field('name',t('m208'),S.renameDraft,'text','required maxlength="40"')}<div class="pz-error" role="alert">${esc(S.error)}</div><button class="pz-primary" type="submit" ${S.renameStatus==='saving'?'disabled':''}>${S.renameStatus==='saving'?a('saving'):t('m209')}</button><p class="pz-small">${c(S.renameScope==='local'?'renameLocalHint':'renameSharedHint')}</p></form>`;}
    if(S.modal==='storage'){title=t('m136');body=storagePanel();}
    if(S.modal==='about'){title=t('m142');body=aboutContent();}
    if(S.modal==='diagnostics-device'){title=targetName(S.target);body=`${deviceInformation(true)}${!hasLink(S.target)?action('connect',t('m186'),'pz-primary',`data-target="${S.target}"`):''}${action('help',c('help'),'pz-secondary')}`;}
    if(S.modal==='rename-product'){title=a('renameProduct');body=`<form id="rename-product-form" novalidate>${field('name',t('m208'),S.productRenameDraft,'text','required maxlength="40"')}<div class="pz-error" role="alert">${esc(S.error)}</div><button class="pz-primary" type="submit">${t('m209')}</button></form>`;}
    if(S.modal==='confirm-apply'){title=u('applyProfile');body=`<p class="pz-sub">${a('confirmApply')}</p><p class="pz-small">${esc(targetName(S.target))}</p>${action('apply-profile',u('applyProfile'))}`;}
    if(S.modal==='delete-map'){title=a('deletePackage');body=action('delete-map',c('delete'),'pz-primary',`data-id="${esc(S.deleteMapId)}"`);}
    if(S.modal==='delete-fence'){title=t('m215');body=`<p class="pz-sub">${t('m216')}</p>${action('confirm-delete',t('m181'),'pz-primary',`data-id="${esc(S.deleteFenceId)}"`)}`;}
    if(S.modal==='apply-target'){title=u('chooseDevice');body=`<div class="pz-card pz-menu-group">${products.filter(p=>p.present).map(p=>menu('select-apply-device',p.type==='chat'?'chat':'gateway',esc(targetName(p.key)),profileCompatible(p,S.pendingApply.config)?linkLabel(p.key):p.identification!=='identified'?c('firmwarePending'):u('incompatible'),`data-target="${p.key}" ${profileCompatible(p,S.pendingApply.config)?'':'disabled'}`)).join('')}</div>`;}
    if(S.modal==='apply-status'){title=t('m133');body=applicationStatus();}
    if(S.modal==='units'){title=u('units');body=`<div class="pz-card pz-menu-group" role="radiogroup">${['metric','imperial'].map(v=>`<button class="pz-menu" role="radio" aria-checked="${S.preferences.units===v}" data-act="set-units" data-value="${v}"><span class="pz-grow">${u(v)}</span>${S.preferences.units===v?'✓':''}</button>`).join('')}</div>`;}
    if(S.modal==='cache'){title=u('storage');body=`<div class="pz-card">${details([[u('mediaCache'),L.number(S.lang,mediaFiles().length)]])}</div>${action('request-clear-cache',u('clearCache'),'pz-secondary',mediaFiles().length?'':'disabled')}`;}
    if(S.modal==='clear-cache'){title=u('clearCache');body=`<p class="pz-sub">${u('clearHint')}</p>${action('clear-cache',u('clearCache'))}`;}
    return `<div class="pz-modal-layer" data-act="backdrop"><section class="pz-sheet ${['radio','fence','language','identity','storage'].includes(S.modal)?'pz-sheet-full':''}" role="dialog" aria-modal="true" aria-label="${title}"><div class="pz-handle"></div><header class="pz-head"><h2>${title}</h2>${action('close',I('close'),'pz-icon-btn',`aria-label="${t('m217')}"`)}</header>${body}</section></div>`;
  }
  function render() {
    const root=$('app'),scroll=S.resetScroll?0:root.querySelector('.pz-page')?.scrollTop||0,sheetScroll=root.querySelector('.pz-sheet')?.scrollTop||0;S.resetScroll=false;
    root.innerHTML=({home,collar,node:nodeDetail,chat,profile,networks,general,notifications,account:accountPage,'reset-password':resetPasswordPage,'device-settings':deviceSettings,about,diagnostics,help}[S.route]||home)()+(['home','profile'].includes(S.route)?`<nav class="pz-nav" aria-label="${t('m218')}">${[['home','home',t('m219')],['profile','settings',t('m128')]].map(([route,icon,label])=>`<button data-act="nav" data-route="${route}" ${S.route===route?'aria-current="page"':''}>${I(icon)}${label}</button>`).join('')}</nav>`:'')+modal();
    const page=root.querySelector('.pz-page');if(page){page.scrollTop=scroll;page.inert=Boolean(S.modal);}
    const sheet=root.querySelector('.pz-sheet');if(sheet)sheet.scrollTop=sheetScroll;
    if($('pz-messages'))$('pz-messages').scrollTop=$('pz-messages').scrollHeight;
    [1,2,3].forEach(n=>$('sim-btn-'+n)?.classList.toggle('active-state',n===S.lifecycle));
    if($('lang-btn-text'))$('lang-btn-text').textContent=L.name(S.lang);
    L.applyStatic(document,S.lang);
    if($('theme-text'))$('theme-text').textContent=document.body.classList.contains('dark-mode')?t('m220'):t('m221');
  }
  const utf8 = value => new TextEncoder().encode(value).length;
  function canonicalRadio(d) {return {country:d.country,halowChannel:Number(d.halowChannel),meshId:d.meshId.trim(),meshPassphrase:d.meshPassphrase};}
  function validateRadio(d) {
    if(!d||typeof d.country!=='string'||typeof d.meshId!=='string'||typeof d.meshPassphrase!=='string')return t('m222');
    if(!catalog[d.country]?.some(c=>c.number===Number(d.halowChannel)&&(!d.bandwidth||c.bandwidthMHz===Number(d.bandwidth))))return t('m223');
    if(!d.meshId.trim()||utf8(d.meshId.trim())>32)return t('m224');
    const length=utf8(d.meshPassphrase);
    return length!==0&&(length<8||length>63)?t('m225'):'';
  }
  for(const p of products){
    const item=saved.schemaVersion===2?saved.radio?.[p.key]:null;
    if(item){const desired=item.desired&&!validateRadio(item.desired)?canonicalRadio(item.desired):null,candidate=item.applied||item.cachedApplied,cachedApplied=candidate&&!validateRadio(candidate)?canonicalRadio(candidate):null;S.radio[p.key]={...freshRadio(),desired,cachedApplied,revision:Number(item.revision)||0,status:desired?'pending':'unknown'};p.config=S.radio[p.key];}
  }
  if(saved.networkProfile?.config&&!validateRadio(saved.networkProfile.config))S.networkProfile={id:'phone-network',config:canonicalRadio(saved.networkProfile.config),revision:Math.max(1,Number(saved.networkProfile.revision)||1),updatedAt:Number(saved.networkProfile.updatedAt)||Date.now()};
  S.radioDraft=withBandwidth(S.networkProfile?.config||{country:'',halowChannel:null,meshId:'',meshPassphrase:''});
  for(const f of S.fences){f.gatewayId=products.find(p=>p.id===f.gatewayId&&p.type==='collar')?.id||'gateway-01';f.deviceIds=(f.deviceIds.length?f.deviceIds:nodes.filter(n=>n.gatewayId===f.gatewayId).map(n=>n.id)).filter(id=>nodes.some(n=>n.id===id&&n.gatewayId===f.gatewayId));f.syncState='local';f.executionTarget='unassigned';f.revision=Number(f.revision)||0;}
  function polygonValid(vertices) {
    if(vertices.length<3||vertices.length>64)return false;
    if(vertices.some(p=>!p||!Number.isFinite(p.latitude)||!Number.isFinite(p.longitude)||Math.abs(p.latitude)>90||Math.abs(p.longitude)>180))return false;
    if(new Set(vertices.map(p=>p.latitude+':'+p.longitude)).size!==vertices.length)return false;
    const orient=(a,b,c)=>(b.longitude-a.longitude)*(c.latitude-a.latitude)-(b.latitude-a.latitude)*(c.longitude-a.longitude);
    let area=0;for(let i=0;i<vertices.length;i++){const a=vertices[i],b=vertices[(i+1)%vertices.length];area+=a.longitude*b.latitude-b.longitude*a.latitude;}
    if(Math.abs(area)<1e-12)return false;
    for(let i=0;i<vertices.length;i++)for(let j=i+1;j<vertices.length;j++) {
      if(j===i+1||(i===0&&j===vertices.length-1))continue;
      const a=vertices[i],b=vertices[(i+1)%vertices.length],c=vertices[j],d=vertices[(j+1)%vertices.length];
      if(orient(a,b,c)*orient(a,b,d)<=0&&orient(c,d,a)*orient(c,d,b)<=0)return false;
    }
    return true;
  }
  function validateFence(f) {
    if(!f.name.trim()||f.name.length>128)return t('m226');
    if(!Number.isFinite(f.center.latitude)||!Number.isFinite(f.center.longitude)||Math.abs(f.center.latitude)>90||Math.abs(f.center.longitude)>180)return t('m227');
    if(!f.enterAlert&&!f.exitAlert)return t('m228');
    const sizes={circle:['radiusMeters'],oval:['radiusXMeters','radiusYMeters'],rectangle:['widthMeters','heightMeters'],polygon:[]}[f.shape];
    if(!sizes)return t('m229');
    if(sizes.some(key=>!Number.isFinite(f[key])||f[key]<10||f[key]>100000))return t('m230');
    if(!Number.isFinite(f.rotationDegrees)||Math.abs(f.rotationDegrees)>180)return t('m231');
    if(f.shape==='polygon'&&!polygonValid(f.vertices))return t('m232');
    if(f.gatewayId!==currentGateway().id||S.fenceScope==='selected'&&!f.deviceIds.length||f.deviceIds.some(id=>!gatewayNodes().some(n=>n.id===id)))return t('m233');
    if(utf8(JSON.stringify(geometryOf(f)))>4000)return t('m234');
    return '';
  }
  function geometryOf(f) {return {center:f.center,rotationDegrees:f.rotationDegrees,...({circle:{radiusMeters:f.radiusMeters},oval:{radiusXMeters:f.radiusXMeters,radiusYMeters:f.radiusYMeters},rectangle:{widthMeters:f.widthMeters,heightMeters:f.heightMeters},polygon:{vertices:f.vertices}})[f.shape]};}
  function openModal(name) {if(name!=='voice'&&media.state.recording)media.discard();S.error='';S.modal=name;render();const focus=$('app').querySelector('[role="dialog"] input, [role="dialog"] button');if(focus)focus.focus();}
  function openRadio(target){S.target=target;openNetwork('profile');}
  function openFence(id){const existing=S.fences.find(f=>f.id===id&&f.gatewayId===currentGateway().id);S.fenceDraft=copy(existing||{...defaultFence,gatewayId:currentGateway().id,id:'f-'+Date.now(),name:'',deviceIds:[]});if(S.fenceDraft.nameKey){S.fenceDraft.name=t(S.fenceDraft.nameKey);delete S.fenceDraft.nameKey;}S.fenceScope=existing?'selected':'all';S.fenceAdvanced=false;S.map={zoom:1,x:0,y:0};openModal('fence');}
  function saveRadio(d){const error=validateRadio(d);if(error){S.error=error;return false;}const item=S.radio[S.target];item.desired=canonicalRadio(d);item.revision++;item.status=item.applied&&JSON.stringify(item.desired)===JSON.stringify(item.applied)?'applied':'pending';persist();S.error='';return true;}
  async function readConfig(){
    const target=S.target,item=S.radio[target],token=(item.readToken||0)+1;item.readToken=token;S.radioReadBusy=true;render();
    try{const value=await adapter.readConfig(target,S.proposal);if(item.readToken!==token)return;if(validateRadio(value))throw new Error('invalid');item.applied=canonicalRadio(value);item.confirmedAt=Date.now();if(!item.desired)item.desired=copy(item.applied);item.status=JSON.stringify(item.desired)===JSON.stringify(item.applied)?'applied':'pending';persist();}
    catch(error){if(S.target===target&&S.modal==='apply-status')S.error=error.message==='unsupported'?a('featurePending'):a('readFailed');}
    finally{if(item.readToken===token)S.radioReadBusy=false;if(S.target===target&&S.modal==='apply-status')render();}
  }
  async function applyRadio(d=S.radioDraft){
    const target=S.target,item=S.radio[target];if(item.inflight)return false;if(!saveRadio(d)){render();return false;}
    const requested=copy(item.desired),revision=item.revision,operation=(item.operation||0)+1;item.operation=operation;item.inflight=true;S.modal='apply-status';render();
    const notify=patch=>{if(item.operation!==operation)return;if(item.revision===revision)Object.assign(item,patch);if(S.modal==='apply-status'&&S.target===target)render();};
    let result;try{result=await adapter.configure(target,requested,notify,S.proposal);}catch{result={status:'failed',error:'failed'};}
    if(item.operation!==operation)return false;item.inflight=false;Object.assign(item,result);
    if(item.revision!==revision)item.status='pending';persist();if(S.modal==='apply-status'&&S.target===target)render();return result.status==='applied';
  }
  function saveFence(f){
    if(S.fenceScope==='all')f.deviceIds=gatewayNodes().map(n=>n.id);
    if(!f.deviceIds.length){S.error=t('m233');return false;}
    if(f.shape==='polygon'&&f.vertices.length)f.center={latitude:f.vertices.reduce((sum,p)=>sum+p.latitude,0)/f.vertices.length,longitude:f.vertices.reduce((sum,p)=>sum+p.longitude,0)/f.vertices.length};
    const error=validateFence(f);if(error){S.error=error;return false;}f.name=f.name.trim();delete f.nameKey;f.revision=(Number(f.revision)||0)+1;f.syncState='local';f.executionTarget='unassigned';f.scope='snapshot';
    const index=S.fences.findIndex(item=>item.id===f.id&&item.gatewayId===f.gatewayId);if(index<0)S.fences.push(copy(f));else S.fences[index]=copy(f);S.acknowledged=S.acknowledged.filter(id=>!id.startsWith(f.id+':'));persist();S.modal=null;S.error='';return true;
  }
  async function syncFence(id){const target=S.activeGateway,f=S.fences.find(f=>f.id===id&&f.gatewayId===productByKey(target).id);if(!f||f.syncState==='syncing')return false;if(!capability('fenceSync',target)){toast(a('featurePending'));return false;}const revision=f.revision;f.syncState='syncing';render();try{const response=await adapter.syncFence(target,copy(f),S.proposal);if(f.revision!==revision||!S.fences.includes(f))return false;Object.assign(f,response);return response.syncState==='monitoring';}catch{if(f.revision===revision)f.syncState='failed';return false;}finally{persist();if(S.route==='collar'&&S.activeGateway===target)render();}}
  async function acknowledge(id){const target=S.activeGateway,alarm=alarms().find(a=>a.id===id);if(!alarm)return;S.ackBusy=id;render();try{await adapter.acknowledge(target,id,S.proposal);if(S.proposal&&!S.acknowledged.includes(id))S.acknowledged.push(id);}catch{toast(a('saveFailed'));}finally{S.ackBusy=null;if(S.activeGateway===target)render();}}
  async function renameNode(name){const id=S.nodeId,target=S.activeGateway,scope=S.renameScope,n=gatewayNodes().find(n=>n.id===id);if(!n)return;S.renameStatus='saving';render();try{await adapter.rename(target,id,name,scope);if(scope==='local')n.alias=name;else n.name=name;persist();if(S.modal==='rename'&&S.nodeId===id&&S.activeGateway===target)S.modal=null;}catch(error){if(S.modal==='rename'&&S.nodeId===id)S.error=error.message==='denied'?t('m323'):a('saveFailed');}finally{S.renameStatus='idle';if(S.nodeId===id&&S.activeGateway===target)render();}}
  async function downloadMap(){if(S.packages.some(p=>['queued','downloading','ready'].includes(p.status)))return;S.packages=S.packages.filter(p=>p.status!=='failed');const pkg={id:'demo-area',status:'queued',progress:0},token=++S.mapEpoch;S.packages.push(pkg);render();try{const result=await adapter.download(value=>{pkg.status='downloading';pkg.progress=value;if(S.modal==='storage')render();},()=>token!==S.mapEpoch);if(token===S.mapEpoch)Object.assign(pkg,result);}catch{if(token===S.mapEpoch)pkg.status='failed';}finally{persist();if(S.modal==='storage')render();}}
  let messageSequence=0;
  async function transmit(m,thread){
    const target=products.find(p=>p.id===m.productId)?.key||S.activeChat;
    delete m.deliveredAt;delete m.readAt;
    await adapter.sendMessage(target,m,{proposal:S.proposal,allowRead:()=>capability('readReceipt',target)},patch=>{if(patch.attemptId){m.attemptIds.push(patch.attemptId);delete patch.attemptId;}Object.assign(m,patch);refreshMessage(m,thread);});
  }
  function send(text,attachment=null) {
    text=String(text||'').trim();if((!text&&!attachment)||!canSend()||attachment?.attachmentKind==='image'&&!capability('chatImage')||attachment?.attachmentKind==='voice'&&!capability('voiceMessage'))return false;
    if(utf8(text)>240){toast(t('m238'));return false;}
    const thread=S.thread,m={productId:productByKey(S.activeChat).id,channelPort:S.thread.startsWith('c')?M.ports[Number(S.thread.slice(1))-1]:null,id:'message-'+Date.now()+'-'+(++messageSequence),mine:true,text,timestampMs:Date.now(),status:'sending',retryCount:0,...(attachment||{})};
    m.logicalMessageId=m.id;m.kind=attachment?.attachmentKind||'text';m.threadId=thread;m.senderId=S.identity.nodeId;m.recipientId=thread.startsWith('c')?String(m.channelPort):peers.find(p=>p.id===thread)?.nodeNumString||thread;m.attemptIds=[];seedConversation(thread);S.composerDrafts[thread]='';S.messages[thread].push(m);void transmit(m,thread);return true;
  }
  function sendMedia(kind){
    if(!canSend()||!media.state.url||media.state.kind!==kind)return;
    if(utf8(S.caption)>240){toast(t('m238'));return;}
    const attachment=media.take(),caption=kind==='image'?S.caption:'';S.caption='';S.modal=null;
    send(caption,attachment);render();
  }
  const handlers={
    'home-area':el=>{if(!HOME_AREAS.some(([id])=>id===el.dataset.value))return;S.homeArea=el.dataset.value;S.resetScroll=true;render();$('area-tab-'+S.homeArea)?.focus();},
    nav:el=>{if(el.dataset.route==='device-settings'&&S.route==='networks'&&S.networkOrigin)S.target=S.networkOrigin;if(el.dataset.route==='chat-list'){S.thread=null;navigate('chat');}else navigate(el.dataset.route);},
    product:el=>{const p=productByKey(el.dataset.value);if(!p){toast(t('m239'));return;}if(p.type==='chat'){S.activeChat=p.key;S.thread=null;navigate('chat');}else{S.activeGateway=p.key;navigate('collar');}},
    add:()=>void startSearch(),'search-again':()=>void startSearch(),
    close:()=>{S.authEpoch=(S.authEpoch||0)+1;S.authBusy=false;S.modal=null;S.error='';media.discard();render();},
    backdrop:(el,event)=>{if(event.target===el){S.modal=null;media.discard();render();}},
    connect:el=>{S.pendingDevice=null;S.target=el.dataset.target||'gateway';openModal('connection');},
    'discovered-device':el=>{const d=discovered.find(d=>d.id===el.dataset.id);if(!d)return;S.pendingDevice=d;S.target=d.target;openModal('connection');},
    pair:async()=>{
      if(S.busy)return;S.busy=true;const candidate=S.pendingDevice||discovered.find(d=>d.target===S.target),target=S.target,p=productByKey(target);S.error='';render();
      try{
        const descriptor=await adapter.identify(candidate||{}),expected=p.type==='chat'?'chatController':'collarGateway';
        if(descriptor.schemaVersion!==1||descriptor.productType!==expected||descriptor.deviceId!==p.id){S.error=u('unknownDevice');return;}
        p.descriptor=descriptor;p.identification='identified';p.connections.ble='ready';p.connections.mesh=p.cache.known?'active':'listening';p.present=true;S.bindings[target]=candidate;if(S.lifecycle===1)S.lifecycle=2;persist();
        if(S.modal==='connection'&&S.target===target){S.modal=null;S.pendingDevice=null;}toast(t('m241'));
      }catch{if(S.target===target)S.error=u('identifyFailed');}
      finally{S.busy=false;render();}
    },
    'choose-image':()=>{if(!canSend())return;if(!capability('chatImage')){toast(c('imagePending'));return;}$('image-picker').click();},
    voice:()=>{if(!canSend()||!capability('voiceMessage'))return;media.discard();S.caption='';openModal('voice');},
    'start-recording':()=>void media.start(),'stop-recording':()=>media.stop(),
    'choose-audio':()=>$('audio-picker').click(),
    'send-image':()=>sendMedia('image'),'send-voice':()=>sendMedia('voice'),
    'view-image':el=>{S.imageMessageId=el.dataset.id;openModal('image-view');},
    'login-mode':el=>{S.authEpoch=(S.authEpoch||0)+1;S.authBusy=false;S.loginMode=el.dataset.value;S.error='';S.accountPassword='';S.showAccountPassword=false;render();},
    settings:el=>{S.target=el.dataset.target||S.target;navigate('device-settings');},networks:()=>openNetwork('profile'),'device-network':()=>openNetwork('device-settings'),general:()=>navigate('general'),notifications:()=>navigate('notifications'),
    'restore-profile':()=>{S.radioDraft=withBandwidth(S.networkProfile.config);S.error='';render();},'application-status':el=>{S.target=el.dataset.target;openModal('apply-status');},
    'select-apply-device':el=>{const p=productByKey(el.dataset.target);if(!S.pendingApply||!profileCompatible(p,S.pendingApply.config))return;S.target=p.key;openModal('confirm-apply');},'apply-profile':()=>applyProfile(),
    'retry-profile':()=>{const item=S.radio[S.target];if(!item.desired)return;S.pendingApply={id:item.profileId||'phone-network',revision:item.profileRevision||0,config:copy(item.desired)};openModal('confirm-apply');},
    units:()=>openModal('units'),'set-units':el=>{if(!['metric','imperial'].includes(el.dataset.value))return;S.preferences.units=el.dataset.value;persist();S.modal=null;render();},cache:()=>openModal('cache'),'request-clear-cache':()=>openModal('clear-cache'),'clear-cache':clearMediaCache,
    'collar-tab':el=>{S.collarTab=el.dataset.value;render();},'chat-tab':el=>{S.chatTab=el.dataset.value;render();},
    node:el=>{S.nodeId=el.dataset.id;navigate('node');},'map-node':el=>{if(S.modal==='fence')return;S.selectedMapNode=el.dataset.id;render();},
    'locate-node':el=>{S.selectedMapNode=el.dataset.id;S.collarTab='map';navigate('collar');},
    'rename-node':()=>{const n=gatewayNodes().find(n=>n.id===S.nodeId);if(!n)return;S.renameScope='local';S.renameDraft=n.alias||nodeName(n);S.renameStatus='idle';openModal('rename');},
    'rename-scope':el=>{S.renameScope=el.dataset.value;const n=gatewayNodes().find(n=>n.id===S.nodeId);S.renameDraft=S.renameScope==='local'?n.alias||nodeName(n):n.name;S.error='';render();},
    'rename-product':()=>{S.productRenameDraft=targetName(S.target);openModal('rename-product');},
    'reload-nodes':()=>{if(S.scenario==='partial')S.scenario='normal';return loadNodes();},'more-nodes':()=>loadNodes(S.activeGateway,true),
    zoom:el=>{if(el.dataset.value==='reset')S.map={zoom:1,x:0,y:0};else S.map.zoom=Math.max(.5,Math.min(4,S.map.zoom*(el.dataset.value==='in'?1.3:1/1.3)));render();},
    'new-fence':()=>openFence(),'edit-fence':el=>openFence(el.dataset.id),'undo-vertex':()=>{S.fenceDraft.vertices.pop();render();},
    'delete-fence':el=>{S.deleteFenceId=el.dataset.id;openModal('delete-fence');},
    'confirm-delete':el=>{S.fences=S.fences.filter(f=>f.id!==el.dataset.id);S.acknowledged=S.acknowledged.filter(id=>!id.startsWith(el.dataset.id+':'));persist();S.modal=null;render();},
    ack:el=>acknowledge(el.dataset.id),'sync-fence':el=>syncFence(el.dataset.id),
    thread:el=>{S.thread=el.dataset.id;seedConversation(S.thread);render();},'thread-info':()=>openModal('thread-info'),
    phrase:el=>send(el.dataset.text),retry:el=>{const m=(S.messages[S.thread]||[]).find(m=>m.id===el.dataset.id);if(m&&m.status==='failed'&&canSend())void transmit(m,S.thread);},
    identity:()=>openModal('identity'),account:()=>navigate('account'),logout:()=>{S.authEpoch=(S.authEpoch||0)+1;S.authBusy=false;for(const p of products)p.connections.cloud='offline';S.account=null;S.otpEmail='';S.accountPassword='';S.accountCode='';S.modal=null;render();toast(t('m242'));},
    language:()=>window.toggleLanguage(),'set-language':el=>{if(!L.has(el.dataset.value))return;S.lang=el.dataset.value;S.modal=null;S.error='';persist();render();},appearance:()=>openModal('appearance'),'set-appearance':el=>{if(!['system','light','dark'].includes(el.dataset.value))return;S.appearance=el.dataset.value;document.body.classList.toggle('dark-mode',S.appearance==='dark'||S.appearance==='system'&&Boolean(window.matchMedia?.('(prefers-color-scheme: dark)').matches));S.modal=null;persist();render();},storage:()=>openModal('storage'),about:()=>navigate('about'),help:()=>navigate('help'),diagnostics:()=>navigate('diagnostics'),'inspect-device':el=>{S.target=el.dataset.target||S.target;openModal('diagnostics-device');},
    'apply-radio':()=>applyRadio(),'read-config':()=>readConfig(),
    'request-apply':()=>{if(!saveNetwork(S.radioDraft)){render();return;}S.pendingApply=copy(S.networkProfile);openModal('apply-target');},
    'reset-password':()=>{S.resetSent=false;navigate('reset-password');},
    'download-map':()=>S.reviewMode?downloadMap():toast(c('mapUnavailable')),'cancel-map':()=>{S.mapEpoch++;S.packages=S.packages.filter(p=>!['queued','downloading'].includes(p.status));render();},
    'request-delete-map':el=>{S.deleteMapId=el.dataset.id;openModal('delete-map');},'delete-map':el=>{S.mapEpoch++;S.packages=S.packages.filter(p=>p.id!==el.dataset.id);persist();openModal('storage');},
    'revert-radio':()=>{if(S.radio[S.target].inflight||!S.radio[S.target].applied)return;S.radioDraft=copy(S.radio[S.target].applied);saveRadio(S.radioDraft);S.radioDraft.bandwidth=catalog[S.radioDraft.country].find(c=>c.number===S.radioDraft.halowChannel).bandwidthMHz;render();},
  };
  $('app').addEventListener('click',event=>{const el=event.target.closest('[data-act]');if(el){if(el.dataset.act==='backdrop'&&event.target!==el)return;event.preventDefault();handlers[el.dataset.act]?.(el,event);}});
  function collectFence(form) {
    const data=new FormData(form),f=copy(S.fenceDraft);
    const numeric=key=>data.get(key)!==null&&String(data.get(key)).trim()!==''?Number(data.get(key)):NaN;
    f.name=String(data.get('name')||'');f.shape=data.get('shape');f.center={latitude:numeric('latitude'),longitude:numeric('longitude')};
    for(const key of ['radiusMeters','radiusXMeters','radiusYMeters','widthMeters','heightMeters','rotationDegrees'])if(data.has(key))f[key]=numeric(key);
    f.enterAlert=data.has('enterAlert');f.exitAlert=data.has('exitAlert');f.enabled=data.has('enabled');f.deviceIds=S.fenceScope==='selected'?data.getAll('deviceIds'):[];return f;
  }
  $('app').addEventListener('submit',async event=>{
    event.preventDefault();const form=event.target,data=new FormData(form);S.error='';
    if(form.id==='chat-form'){if(!String(data.get('message')||'').trim())toast(t('m114'));else send(data.get('message'));return;}
    if(form.id==='radio-form'){if(saveNetwork(S.radioDraft))toast(a('savedLocally'));render();return;}
    if(form.id==='fence-form'){if(saveFence(collectFence(form)))toast(a('savedLocally'));render();return;}
    if(['identity-form','rename-form','rename-product-form'].includes(form.id)){
      const name=String(data.get('name')||'').trim();if(!name||name.length>40){S.error=t('m245');render();return;}
      if(form.id==='rename-form'){S.renameDraft=name;await renameNode(name);return;}
      if(form.id==='identity-form')S.identity.name=name;else productByKey(S.target).alias=name;
      persist();S.modal=null;render();return;
    }
    if(form.id==='account-form'||form.id==='reset-form'){
      if(S.authBusy)return;const email=String(data.get('email')||'').trim();if(!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)){S.error=t('m246');render();return;}
      const mode=S.loginMode,token=(S.authEpoch||0)+1;S.authEpoch=token;S.authBusy=true;S.accountEmail=email;render();
      try{
        if(form.id==='reset-form'){S.resetEmail=email;await adapter.resetPassword(email,S.proposal);if(token===S.authEpoch)S.resetSent=true;}
        else{const requestOnly=mode==='code'&&email!==S.otpEmail,result=await adapter.authenticate({email,mode,secret:data.get(mode==='password'?'password':'otp'),proposal:S.proposal,requestOnly});if(token!==S.authEpoch)return;if(requestOnly){S.otpEmail=email;toast(t('m247'));}else{S.account=result;S.accountPassword='';S.accountCode='';toast(t('m249'));}}
      }catch(error){if(token===S.authEpoch){S.error=error.message==='unsupported'?a('featurePending'):error.message==='credentials'?t(mode==='password'?'m302':'m248'):a('saveFailed');S.accountPassword='';}}
      finally{if(token===S.authEpoch){S.authBusy=false;render();}}return;
    }
  });
  $('app').addEventListener('input',event=>{
    const el=event.target;
    if(el.name==='caption')S.caption=el.value;
    if(el.closest('#rename-form')&&el.name==='name')S.renameDraft=el.value;
    if(el.closest('#rename-product-form')&&el.name==='name')S.productRenameDraft=el.value;
    if(el.closest('#reset-form')&&el.name==='email')S.resetEmail=el.value;
    if(el.closest('#account-form')){if(el.name==='email')S.accountEmail=el.value;if(el.name==='password')S.accountPassword=el.value;if(el.name==='otp')S.accountCode=el.value;}
    if(el.id==='message-input')S.composerDrafts[S.thread]=el.value;
    if(el.id==='node-search'){S.search=el.value;$('node-results').innerHTML=nodeResults(filteredNodes(),gatewayNodes());}
    if(el.closest('#radio-form')&&['meshId','meshPassphrase'].includes(el.name))S.radioDraft[el.name]=el.value;
    if(el.closest('#fence-form')&&el.name==='name')S.fenceDraft.name=el.value;
  });
  $('app').addEventListener('change',event=>{
    const el=event.target;
    if(['messages','fences','sound','vibration'].includes(el.dataset?.preference)){S.preferences.notifications[el.dataset.preference]=Boolean(el.checked);persist();return;}
    if(el.id==='image-picker'||el.id==='audio-picker'){const kind=el.id==='image-picker'?'image':'voice';if(kind==='image'&&!capability('chatImage')||kind==='voice'&&!capability('voiceMessage'))return;void media.choose(el.files?.[0],kind).then(ok=>{if(ok){S.caption='';openModal(kind==='image'?'compose-image':'voice');}});return;}
    if(el.id==='show-account-password'){S.showAccountPassword=el.checked;const input=$('account-form').querySelector('[name="password"]');if(input)input.type=el.checked?'text':'password';}
    if(el.id==='node-filter'){S.filter=el.value;render();}
    if(el.id==='show-pass'){S.showPass=el.checked;const input=$('radio-form').querySelector('[name="meshPassphrase"]');input.type=el.checked?'text':'password';}
    if(el.closest('#radio-form')&&['country','bandwidth','halowChannel'].includes(el.name)) {
      const d=S.radioDraft;S.radioAdvanced=Boolean($('radio-form').querySelector('details')?.open);if(el.name==='country'){d.country=el.value;const c=catalog[d.country]?.[0];if(!c){d.halowChannel=null;d.bandwidth=null;render();return;}d.halowChannel=c.number;d.bandwidth=c.bandwidthMHz;}
      else if(el.name==='bandwidth'){d.bandwidth=Number(el.value);d.halowChannel=catalog[d.country].find(c=>c.bandwidthMHz===d.bandwidth).number;}
      else d.halowChannel=Number(el.value);S.error='';render();
    }
    if(el.closest('#fence-form')) {
      const form=el.closest('form');S.fenceAdvanced=Boolean(form.querySelector('[data-fence-advanced]')?.open);S.fenceDraft=collectFence(form);
      if(el.name==='scope'){S.fenceScope=el.value;if(el.value==='all')S.fenceDraft.deviceIds=[];}
      S.error='';render();
    }
  });
  let drag=null;
  $('app').addEventListener('pointerdown',event=>{
    const svg=event.target.closest('svg[data-map]');if(!svg||event.target.closest('[data-act="map-node"]'))return;
    const rect=svg.getBoundingClientRect();drag={id:event.pointerId,x:event.clientX,y:event.clientY,originX:S.map.x,originY:S.map.y,rect,svg};svg.setPointerCapture?.(event.pointerId);
  });
  $('app').addEventListener('pointermove',event=>{
    if(!drag)return;S.map.x=drag.originX+(event.clientX-drag.x)*360/drag.rect.width;S.map.y=drag.originY+(event.clientY-drag.y)*300/drag.rect.height;
    drag.svg.querySelector('[data-map-content]')?.setAttribute('transform',`translate(${S.map.x} ${S.map.y}) translate(180 150) scale(${S.map.zoom}) translate(-180 -150)`);
  });
  $('app').addEventListener('pointerup',event=>{
    if(!drag)return;const d=drag;drag=null;
    if(Math.hypot(event.clientX-d.x,event.clientY-d.y)<5&&S.modal==='fence') {
      const x=((event.clientX-d.rect.left)*360/d.rect.width-S.map.x-180)/S.map.zoom+180,y=((event.clientY-d.rect.top)*300/d.rect.height-S.map.y-150)/S.map.zoom+150,p=pointGeo(x,y);
      if(S.fenceDraft.shape==='polygon'){if(S.fenceDraft.vertices.length<64)S.fenceDraft.vertices.push(p);else toast(t('m250'));}else S.fenceDraft.center=p;
    }render();
  });
  $('app').addEventListener('pointercancel',()=>{drag=null;});
  $('app').addEventListener('keydown',event=>{
    const area=event.target.closest('[data-act="home-area"]');
    if(area&&['ArrowLeft','ArrowRight','Home','End'].includes(event.key)){
      event.preventDefault();const index=HOME_AREAS.findIndex(([id])=>id===S.homeArea),next=event.key==='Home'?0:event.key==='End'?HOME_AREAS.length-1:(index+(event.key==='ArrowRight'?1:-1)+HOME_AREAS.length)%HOME_AREAS.length;
      handlers['home-area']({dataset:{value:HOME_AREAS[next][0]}});return;
    }
    if(event.key==='Escape'&&S.modal){S.modal=null;media.discard();render();}
    const marker=event.target.closest('.pz-marker');if(marker&&['Enter',' '].includes(event.key)){event.preventDefault();handlers['map-node'](marker);}
    if(event.key==='Tab'&&S.modal){const list=[...$('app').querySelectorAll('[role="dialog"] button:not(:disabled),[role="dialog"] input,[role="dialog"] select')];if(event.shiftKey&&document.activeElement===list[0]){event.preventDefault();list.at(-1)?.focus();}else if(!event.shiftKey&&document.activeElement===list.at(-1)){event.preventDefault();list[0]?.focus();}}
  });
  function setScene(state){
    media.discard();S.lifecycle=state;S.modal=null;S.scenario='normal';
    for(const p of products){p.descriptor=state>1?M.demoDescriptor(p.key):null;p.identification=state>1?'identified':'unidentified';p.cache.request++;p.cache.known=state===3&&p.type==='collar'&&p.present;p.cache.limit=4;p.cache.status=p.cache.known?'ready':'idle';p.cache.error=null;p.permission='write';p.connections={ble:state===1?'not-selected':'ready',mesh:state===3?'active':state===2?'listening':'unknown',cloud:'offline',lastError:null};}
    if(state===3){for(const n of nodes){const ageMs=n.id==='n04'?1100000:n.id==='n06'?900000:30000;n.lastSeenAt=Date.now()-ageMs;n.observations.signal.sampleAt=n.lastSeenAt;n.observations.signal.receivedAt=n.lastSeenAt;if(n.latitude!==null){n.observations.position.sampleAt=n.lastSeenAt;n.observations.position.receivedAt=n.lastSeenAt;}}}
    if(state!==3&&S.route==='node')S.route='collar';persist();render();
  }
  function setScenario(value){setScene(3);S.scenario=value;const p=currentGateway();
    if(value==='two'){productByKey('gateway2').present=true;productByKey('gateway2').cache.known=true;productByKey('gateway2').cache.status='ready';}
    if(value==='ble-down'){for(const item of products){item.connections.ble='disconnected';item.connections.mesh='unknown';item.cache.status=item.cache.known?'cached':'idle';}}
    if(value==='cloud-only'){S.account={email:'ranch@example.com',name:identityName(),method:'scenario'};for(const item of products){item.connections.ble='disconnected';item.connections.cloud='online';}}
    if(value==='denied'){p.permission='denied';p.cache.status='denied';}
    if(value==='partial')p.cache.status='partial';persist();render();
  }
  window.setSimState=setScene;
  $('demo-scenario')?.addEventListener('change',event=>setScenario(event.target.value));
  $('demo-contract')?.addEventListener('change',event=>{media.discard();S.proposal=event.target.value==='proposal';S.modal=null;S.error='';S.authEpoch=(S.authEpoch||0)+1;S.authBusy=false;if(!S.proposal){for(const item of Object.values(S.radio)){item.operation=(item.operation||0)+1;item.readToken=(item.readToken||0)+1;item.inflight=false;if(item.applied)item.cachedApplied=copy(item.applied);item.applied=null;item.confirmedAt=null;item.status=item.desired?'pending':'unknown';}if(S.account?.method==='password')S.account=null;for(const f of S.fences){f.syncState='local';f.executionTarget='unassigned';f.revision++;}for(const list of Object.values(S.messages))for(const m of list)if(m.status==='read'){m.status='delivered';delete m.readAt;}S.acknowledged=[];}render();});
  window.toggleLanguage = () => openModal('language');
  window.toggleTheme = () => {document.body.classList.toggle('dark-mode');S.appearance=document.body.classList.contains('dark-mode')?'dark':'light';persist();render();};
  window.openSetupWizard = () => void startSearch();
  // The presentation exposes a small inspectable model for reproducible handoff verification.
  window.EdgezBoard={model:M,products,adapter,productByKey,gatewayNodes,liveNodes,loadNodes,setScenario,setScene,readConfig,syncFence,acknowledge,renameNode,downloadMap,capability,media,startSearch,discovered,state:S,nodes,catalog,render,navigate,send,saveRadio,applyRadio,saveFence,validateRadio,validateFence,contains,geometryOf,openRadio,openFence,handlers,alarms,saveNetwork,openNetwork,applyProfile,profileCompatible,notificationPolicy,notifyEvent,receiveMessage,distance,clearMediaCache};
  for(const f of S.fences)if(f.id==='f1'&&f.name==='牧区 A')f.nameKey='m259';
  const hash=window.location.hash;
  if(hash==='#profile')S.route='profile';else if(hash==='#chat')S.route='chat';else if(hash==='#collar')S.route='collar';
  if(saved.appearance==='dark'||!saved.appearance&&window.matchMedia?.('(prefers-color-scheme: dark)').matches)document.body.classList.add('dark-mode');
  window.matchMedia?.('(prefers-color-scheme: dark)').addEventListener?.('change',event=>{if(S.appearance==='system'){document.body.classList.toggle('dark-mode',event.matches);render();}});
  render();
})();
