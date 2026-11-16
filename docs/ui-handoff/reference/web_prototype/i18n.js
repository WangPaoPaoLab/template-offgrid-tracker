/* Every locale ships locally. Interface language never changes the radio region. */
(() => {
  'use strict';
  const languages = [
    {code:'zh',locale:'zh-CN',name:'简体中文'},
    {code:'zh-Hant',locale:'zh-TW',name:'繁體中文'},
    {code:'en',locale:'en-US',name:'English'},
    {code:'fr',locale:'fr-FR',name:'Français'},
    {code:'de',locale:'de-DE',name:'Deutsch'},
    {code:'es',locale:'es-ES',name:'Español'},
    {code:'it',locale:'it-IT',name:'Italiano'},
    {code:'pt',locale:'pt-BR',name:'Português'},
    {code:'nl',locale:'nl-NL',name:'Nederlands'},
    {code:'ja',locale:'ja-JP',name:'日本語'},
    {code:'ko',locale:'ko-KR',name:'한국어'},
    {code:'hi',locale:'hi-IN',name:'हिन्दी'},
  ];
  const data=window.EdgezLocaleData;
  const get=code=>languages.find(item=>item.code===code);
  const locale=code=>(get(code)||get('en')).locale;
  const number=(code,value,options={})=>new Intl.NumberFormat(locale(code),options).format(value);
  function t(code,key,params={}) {
    const lang=get(code)?code:'en',index=Number(key.slice(1)),text=data[lang]?.[index];
    if(!/^m\d{3}$/.test(key)||typeof text!=='string')throw new Error('Missing translation: '+lang+'/'+key);
    return text.replace(/\{(p\d+)\}/g,(_,name)=>{
      if(!(name in params))throw new Error('Missing translation parameter: '+key+'/'+name);
      return typeof params[name]==='number'?number(lang,params[name]):String(params[name]);
    });
  }
  function applyStatic(document,code) {
    document.querySelectorAll?.('[data-i18n]').forEach(el=>{el.textContent=t(code,el.dataset.i18n);});
    document.querySelectorAll?.('[data-i18n-aria]').forEach(el=>el.setAttribute('aria-label',t(code,el.dataset.i18nAria)));
    document.documentElement?.setAttribute('lang',locale(code));
    document.title=t(code,'m279');
  }
  function region(code,regionCode) {
    try{return new Intl.DisplayNames([locale(code)],{type:'region'}).of(regionCode);}catch{return regionCode;}
  }
  window.EdgezI18n={languages,has:code=>Boolean(get(code)),locale,number,t,applyStatic,region,name:code=>get(code)?.name||'English'};
})();
