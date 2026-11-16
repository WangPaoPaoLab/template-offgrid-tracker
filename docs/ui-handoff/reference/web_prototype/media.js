/* Local file selection and microphone capture. This module sends nothing over a network. */
(() => {
  window.EdgezMedia={create({language,notify,onChange}){
    const state={kind:null,url:null,name:'',mime:'',size:0,durationMs:0,recording:false,requesting:false,seconds:0};
    let recorder=null,stream=null,timer=null,epoch=0,startedAt=0;
    const t=key=>window.EdgezI18n.t(language(),key);
    function stopTracks(){stream?.getTracks().forEach(track=>track.stop());stream=null;clearInterval(timer);timer=null;}
    function discard(){epoch++;if(recorder&&recorder.state!=='inactive')recorder.stop();recorder=null;stopTracks();if(state.url)URL.revokeObjectURL(state.url);Object.assign(state,{kind:null,url:null,name:'',mime:'',size:0,durationMs:0,recording:false,requesting:false,seconds:0});}
    async function choose(file,kind){
      if(!file)return false;
      if(file.size<=0){notify(t('m305'));return false;}
      if(file.size>8*1024*1024){notify(t('m291'));return false;}
      if(kind==='image'&&!['image/jpeg','image/png','image/webp','image/gif'].includes(file.type)){notify(t('m292'));return false;}
      if(kind==='voice'&&!file.type.startsWith('audio/')){notify(t('m295'));return false;}
      discard();const token=epoch,url=URL.createObjectURL(file);
      if(kind==='image'&&typeof window.Image==='function'){
        try{const img=new Image();img.src=url;await img.decode();}catch{URL.revokeObjectURL(url);notify(t('m305'));return false;}
      }
      if(token!==epoch){URL.revokeObjectURL(url);return false;}
      Object.assign(state,{kind,url,name:file.name||t(kind==='image'?'m281':'m282'),mime:file.type,size:file.size,durationMs:0});
      if(kind==='voice'&&typeof window.Audio==='function'){
        const audio=new Audio();audio.preload='metadata';audio.onloadedmetadata=()=>{if(state.url===url&&Number.isFinite(audio.duration)){state.durationMs=audio.duration*1000;onChange('preview');}};audio.src=url;
      }
      onChange('preview');return true;
    }
    async function start(){
      if(state.requesting||state.recording)return false;
      discard();const token=epoch;
      if(!window.navigator?.mediaDevices?.getUserMedia||typeof window.MediaRecorder!=='function'){notify(t('m293'));return false;}
      let acquired;
      try{
        state.requesting=true;onChange('recording');
        acquired=await navigator.mediaDevices.getUserMedia({audio:true});
        if(token!==epoch){acquired.getTracks().forEach(track=>track.stop());return false;}
        state.requesting=false;
        stream=acquired;
        const mime=['audio/webm;codecs=opus','audio/ogg;codecs=opus','audio/mp4'].find(type=>MediaRecorder.isTypeSupported(type));
        recorder=new MediaRecorder(stream,mime?{mimeType:mime}:{});const chunks=[];
        recorder.ondataavailable=event=>{if(event.data.size)chunks.push(event.data);};
        recorder.onerror=()=>{if(token!==epoch)return;discard();notify(t('m311'));onChange('preview');};
        recorder.onstop=()=>{
          const durationMs=Date.now()-startedAt;stopTracks();if(token!==epoch)return;
          const type=recorder?.mimeType||chunks[0]?.type||'audio/webm',blob=new Blob(chunks,{type});recorder=null;
          if(!blob.size||blob.size>8*1024*1024){state.recording=false;notify(t(blob.size?'m291':'m311'));onChange('preview');return;}
          Object.assign(state,{kind:'voice',url:URL.createObjectURL(blob),name:'voice-message',mime:type,size:blob.size,durationMs,recording:false});onChange('preview');
        };
        startedAt=Date.now();state.recording=true;state.kind='voice';state.seconds=0;recorder.start();onChange('recording');
        timer=setInterval(()=>{state.seconds=Math.floor((Date.now()-startedAt)/1000);onChange('tick');if(state.seconds>=60)stop();},250);
        return true;
      }catch(error){acquired?.getTracks().forEach(track=>track.stop());if(token!==epoch)return false;stopTracks();state.recording=false;state.requesting=false;notify(t(error?.name==='NotAllowedError'?'m294':'m311'));onChange('preview');return false;}
    }
    function stop(){if(recorder&&recorder.state==='recording')recorder.stop();}
    function take(){if(!state.url||state.recording)return null;const result={attachmentKind:state.kind,objectUrl:state.url,attachmentName:state.name,attachmentMimeType:state.mime,attachmentSize:state.size,durationMs:state.durationMs};state.url=null;discard();return result;}
    return {state,choose,start,stop,discard,take};
  }};
})();
