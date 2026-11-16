/* Demo adapter and frontend contracts. Never sends BLE, HTTP or cloud commands. */
(() => {
  'use strict';
  const clone = value => JSON.parse(JSON.stringify(value));
  const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
  const ports = [38801,38803,38805,38807,38809];
  const radioDefaults = {country:'US',halowChannel:27,meshId:'KEEPTEEN',meshPassphrase:''};
  const transaction = () => ({desired:null,applied:null,status:'unknown',revision:0,requestId:null,confirmedAt:null,error:null});
  const product = (key,id,type,nameKey) => ({key,id,type,nameKey,alias:'',present:key!=='gateway2',model:type==='collar'?'HaLow gateway':'Chat controller',firmwareVersion:null,firmwareTarget:'unverified-demo',controlDeviceId:id,connections:{ble:'not-selected',mesh:'unknown',cloud:'offline',lastError:null},capabilities:{radioRead:null,radioWrite:true,chatText:type==='chat',voiceMessage:type==='chat',chatImage:false,readReceipt:false,fenceSync:false,passwordLogin:false},config:transaction(),cache:{known:false,limit:4,status:'idle',request:0,error:null},permission:'write'});
  function createProducts(){return [product('gateway','gateway-01','collar','m015'),product('radio','chat-01','chat','m001'),product('gateway2','gateway-02','collar','m015')];}
  // Proposed identity handshake, represented only by explicit fixtures in this web board.
  // An advertisement prefix/MCU model is never sufficient to infer a product type.
  function demoDescriptor(key){return {schemaVersion:1,deviceId:key==='radio'?'chat-01':key==='gateway2'?'gateway-02':'gateway-01',productType:key==='radio'?'chatController':'collarGateway',modelId:key==='radio'?'fixture-chat':'fixture-gateway',firmwareRole:key==='radio'?'chat-controller':'collar-gateway',firmwareVersion:null,capabilities:{radioWrite:true,chatText:key==='radio',voiceMessage:key==='radio'},radioTuples:Object.entries(window.EdgezRadioCatalog).flatMap(([country,rows])=>rows.map(r=>({country,channel:r.number}))),source:'demo-fixture'};}
  function enrichNodes(rows,now) {
    const result=rows.map((row,i)=>{
      const [id,name,type,batteryVoltage,rssi,latitude,longitude,age]=row;
      const gpsOnly=id==='n08',interval=id==='n06'?900000:id==='n07'?null:60000;
      const seen=now-(id==='n04'?1100:age)*1000;
      const node={id,nodeId:id,cloudDeviceId:'demo-device-'+id,serial:'DEMO-'+name,nodeNumString:(9007199254741000n+BigInt(i)).toString(),name,alias:'',hardwareName:name,type,gatewayId:'gateway-01',model:gpsOnly?'FGH200M GPS_ONLY':'Unverified sensor',expectedIntervalMs:interval,graceFactor:3,capabilities:{gps:true,batteryVoltage:gpsOnly?false:null},observations:{battery:{value:batteryVoltage,unit:'V',sampleAt:null,receivedAt:seen,source:'cloud-cache',validity:batteryVoltage===null?'missing':'cached'},signal:{value:rssi,unit:'dBm',sampleAt:seen,receivedAt:seen,source:'halow',validity:rssi===null?'missing':'valid',reporterId:'gateway-01',peerId:id},position:{value:latitude===null?null:{latitude,longitude},sampleAt:latitude===null?null:seen,receivedAt:seen,source:'gps',validity:latitude===null?'missing':'valid',coordinateSystem:'WGS84'}},lastSeenAt:seen,lowBattery:null,settingsRanges:gpsOnly?{samplingIntervalMs:[60000],blockSamplingCount:[0]}:null};
      // Compatibility getters keep the previous map/geometry code free of transport details.
      Object.defineProperties(node,{batteryVoltage:{enumerable:true,get(){return this.capabilities.batteryVoltage===false?null:this.observations.battery.value;}},rssi:{enumerable:true,get(){return this.observations.signal.value;}},latitude:{enumerable:true,get(){return this.observations.position.value?.latitude??null;}},longitude:{enumerable:true,get(){return this.observations.position.value?.longitude??null;}},locationAt:{enumerable:true,get(){return this.observations.position.sampleAt;}}});
      return node;
    });
    for(const [id,name,lat] of [['n09','102',31.233],['n10','108',31.234]]){
      const n=clone(result[0]);n.id=n.nodeId=id;n.name=n.hardwareName=name;n.serial='DEMO-'+name;n.cloudDeviceId='demo-device-'+id;n.nodeNumString=(9007199254741010n+BigInt(result.length)).toString();n.gatewayId='gateway-02';n.observations.signal.reporterId=n.gatewayId;n.observations.signal.peerId=id;n.observations.position.value={latitude:lat,longitude:121.478};
      for(const key of ['batteryVoltage','rssi','latitude','longitude','locationAt'])delete n[key];
      const template=result[0];for(const key of ['batteryVoltage','rssi','latitude','longitude','locationAt'])Object.defineProperty(n,key,Object.getOwnPropertyDescriptor(template,key));
      result.push(n);
    }
    return result;
  }
  function freshness(node,now=Date.now()){
    if(!node.lastSeenAt)return 'missing';
    if(!node.expectedIntervalMs)return 'unknown';
    return now-node.lastSeenAt<=node.expectedIntervalMs*node.graceFactor?'fresh':'stale';
  }
  function positionFresh(node,now=Date.now()) {return node.observations.position.validity==='valid'&&node.locationAt!==null&&Boolean(node.expectedIntervalMs)&&now-node.locationAt<=node.expectedIntervalMs*node.graceFactor;}
  const ready=p=>p?.connections.ble==='ready';
  const accessible=p=>ready(p)||p?.connections.cloud==='online';
  function capabilities(p,proposal){return {...p.capabilities,...(proposal?{radioRead:true,chatImage:p.type==='chat',readReceipt:p.type==='chat',fenceSync:p.type==='collar',passwordLogin:true}:{})};}
  function demoAdapter({products,nodes,outcome}){
    let sequence=0;
    const get=key=>products.find(p=>p.key===key||p.id===key);
    async function identify(device){await delay(180);const result=outcome('identify');if(result==='failed')throw new Error('identify-failed');if(result==='unknown')return {schemaVersion:1,productType:'unknown'};const key=({'DEMO-GW-01':'gateway','DEMO-GW-02':'gateway2','DEMO-CHAT-01':'radio'})[device.id];return key?demoDescriptor(key):{schemaVersion:1,productType:'unknown'};}
    async function listNodes(key){const p=get(key);await delay(160);if(!accessible(p))throw new Error('offline');if(p.permission==='denied'||outcome('nodes')==='denied')throw new Error('denied');if(outcome('nodes')==='failed')throw new Error('failed');return {rows:nodes.filter(n=>n.gatewayId===p.id),total:nodes.filter(n=>n.gatewayId===p.id).length};}
    async function rename(key,id,name,scope){const p=get(key);await delay(220);if(scope==='shared'&&(p.connections.cloud!=='online'||p.permission!=='write'))throw new Error('denied');if(outcome('rename')==='failed')throw new Error('failed');return {id,name,scope};}
    async function configure(key,desired,onState,proposal){const p=get(key),requestId='demo-config-'+(++sequence);if(p.permission!=='write')return {status:'failed',requestId,error:'denied'};onState({status:'cached',requestId});await delay(180);if(!accessible(p))return {status:'cached',requestId};onState({status:'transmitting',requestId});await delay(260);if(!accessible(p))return {status:'cached',requestId};const result=outcome('config');if(result==='failed'||result==='expired')return {status:result,requestId,error:result};if(!capabilities(p,proposal).radioRead)return {status:'cached',requestId,error:'confirmation-unavailable'};return {status:'applied',requestId,applied:clone(desired),confirmedAt:Date.now()};}
    async function readConfig(key,proposal){const p=get(key);await delay(180);if(!capabilities(p,proposal).radioRead)throw new Error('unsupported');if(!accessible(p))throw new Error('offline');if(p.permission==='denied')throw new Error('denied');return clone(p.config.applied||p.config.cachedApplied||radioDefaults);}
    async function syncFence(key,fence,proposal){const p=get(key);await delay(250);if(!capabilities(p,proposal).fenceSync)throw new Error('unsupported');if(p.connections.cloud!=='online')return {syncState:'pending',executionTarget:'cloud'};if(p.permission!=='write'||outcome('fence')==='denied')throw new Error('denied');if(outcome('fence')==='failed')throw new Error('failed');return {syncState:'monitoring',executionTarget:'cloud',revision:fence.revision,confirmedAt:Date.now()};}
    async function acknowledge(key,alarm,proposal){const p=get(key);await delay(180);if(!capabilities(p,proposal).fenceSync)throw new Error('unsupported');if(p.connections.cloud!=='online'||p.permission!=='write'||outcome('fence')==='denied')throw new Error('denied');return {id:alarm,acknowledgedAt:Date.now()};}
    async function authenticate({email,mode,secret,proposal,requestOnly}){await delay(180);if(mode==='password'&&!proposal)throw new Error('unsupported');if(outcome('auth')==='failed')throw new Error('failed');if(requestOnly)return {userId:'demo-user',email};if(secret!==(mode==='password'?'Demo@123456':'123456'))throw new Error('credentials');return {email,name:email.split('@')[0],method:mode};}
    async function resetPassword(email,proposal){await delay(180);if(!proposal)throw new Error('unsupported');if(outcome('auth')==='failed')throw new Error('failed');return {email,requested:true};}
    async function sendMessage(key,message,{proposal,allowRead},onState){
      const p=get(key),supported=capabilities(p,proposal),direct=message.channelPort===null;
      const connected=()=>ready(p)&&(p.connections.mesh==='active'||!direct&&p.connections.mesh==='listening');
      if(message.kind==='image'&&!supported.chatImage||message.kind==='voice'&&!supported.voiceMessage){onState({status:'failed',error:'unsupported'});return;}
      onState({status:'sending',retryCount:0,attemptId:'demo-attempt-'+(++sequence)});
      await delay(350);if(!connected()){onState({status:'failed'});return;}onState({status:'sent'});
      if(!direct)return;
      const result=outcome('message');
      if(result==='delivered'||result==='read'){
        await delay(250);if(!connected()){onState({status:'failed'});return;}onState({status:'delivered',deliveredAt:Date.now()});
        if(result==='read'&&supported.readReceipt){await delay(500);if(connected()&&allowRead())onState({status:'read',readAt:Date.now()});}return;
      }
      for(let retryCount=1;retryCount<=3;retryCount++){if(!connected()){onState({status:'failed'});return;}onState({status:'retrying',retryCount,attemptId:'demo-attempt-'+(++sequence)});await delay(600);}onState({status:'failed'});
    }
    async function download(onProgress,cancelled){for(let step=1;step<=5;step++){await delay(180);if(cancelled())return {status:'cancelled'};if(outcome('map')==='failed'&&step===3)throw new Error('failed');onProgress(step*20);}return {status:'ready'};}
    return {kind:'demo',identify,listNodes,rename,configure,readConfig,syncFence,acknowledge,authenticate,resetPassword,sendMessage,download};
  }
  window.EdgezAlignment={ports,radioDefaults,transaction,createProducts,demoDescriptor,enrichNodes,freshness,positionFresh,ready,accessible,capabilities,demoAdapter};
})();
