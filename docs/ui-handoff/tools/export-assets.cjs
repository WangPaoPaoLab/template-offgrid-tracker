/* Export the current code-native SVG artwork without redrawing the brand or products. */
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),crypto=require('node:crypto');
const {Resvg}=require('@resvg/resvg-js');
const root=path.resolve(__dirname,'..');
const bundled=path.join(root,'reference/web_prototype');
const web=process.env.KEEPTEEN_WEB_SOURCE?path.resolve(process.env.KEEPTEEN_WEB_SOURCE):fs.existsSync(bundled)?bundled:path.resolve(root,'../web_prototype');
const themes={light:{stroke:'#5B5B61',accent:'#FF8500',fill:'#EFEFF2',card:'#FFFFFF'},dark:{stroke:'#C7C7CC',accent:'#FF9A33',fill:'#2C2C2E',card:'#1C1C1E'}};
const manifest=[];
const previousManifest=fs.existsSync(path.join(root,'ASSET_MANIFEST.json'))?JSON.parse(fs.readFileSync(path.join(root,'ASSET_MANIFEST.json'),'utf8')).files:[];
const sha=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const relative=file=>path.relative(root,file).split(path.sep).join('/');
function write(file,bytes,meta={}){const target=path.join(root,file);fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,bytes);manifest.push({path:relative(target),...meta,bytes:Buffer.byteLength(bytes),sha256:sha(bytes)});}
function alphaStats(image){let transparent=0,visible=0;const p=image.pixels;for(let i=3;i<p.length;i+=4){if(p[i]===0)transparent++;else visible++;}return {hasTransparency:transparent>0,hasArtwork:visible>0};}
function render(svg,width){return new Resvg(svg,{fitTo:{mode:'width',value:width},font:{loadSystemFonts:false}}).render();}
const catalog=path.join(root,'platforms/ios/KEEPTEENAssets.xcassets');
fs.mkdirSync(catalog,{recursive:true});
fs.writeFileSync(path.join(catalog,'Contents.json'),JSON.stringify({info:{author:'KEEPTEEN UI handoff',version:1}},null,2)+'\n');
function exportAsset({id,category,name,svgByTheme,width,height,sourceKey}){
  const ios=[];
  for(const [theme,svg] of Object.entries(svgByTheme)){
    const common={assetId:id,category,theme,sourceKey};
    write(`assets/${category}/svg/${theme}/${name}.svg`,svg,{...common,format:'svg',width,height,vector:category!=='brand'||name==='edgez_logo'});
    const outputs=new Map();
    for(const scale of [1,1.5,2,3,4]){
      const image=render(svg,Math.round(width*scale)),png=image.asPng(),alpha=alphaStats(image);
      if(!alpha.hasArtwork||!alpha.hasTransparency)throw new Error('Expected transparent artwork: '+id+'/'+theme);
      outputs.set(scale,{png,width:image.width,height:image.height,...alpha});
    }
    for(const scale of [1,2,3]){
      const item=outputs.get(scale),base=scale===1?name:`${name}@${scale}x`;
      write(`assets/${category}/png/${theme}/${base}.png`,item.png,{...common,format:'png',width:item.width,height:item.height,scale,hasTransparency:item.hasTransparency});
      const filename=`${name}_${theme}${scale===1?'':`@${scale}x`}.png`;
      write(`platforms/ios/KEEPTEENAssets.xcassets/${id}.imageset/${filename}`,item.png,{...common,format:'png',platform:'ios',width:item.width,height:item.height,scale,hasTransparency:true});
      ios.push({idiom:'universal',filename,scale:`${scale}x`,...(theme==='dark'?{appearances:[{appearance:'luminosity',value:'dark'}]}:{})});
    }
    for(const [density,scale] of [['mdpi',1],['hdpi',1.5],['xhdpi',2],['xxhdpi',3],['xxxhdpi',4]]){
      const item=outputs.get(scale);
      write(`platforms/android/drawable-${theme==='dark'?'night-':''}${density}/${id}.png`,item.png,{...common,format:'png',platform:'android',density,width:item.width,height:item.height,scale,hasTransparency:true});
    }
  }
  const contents={images:ios,info:{author:'KEEPTEEN UI handoff',version:1}};
  write(`platforms/ios/KEEPTEENAssets.xcassets/${id}.imageset/Contents.json`,JSON.stringify(contents,null,2)+'\n',{assetId:id,category,format:'json',platform:'ios'});
}
function fixedSvg(source,theme,category){
  const t=themes[theme],product=category==='products';
  return source.replace('<svg ',`<svg xmlns="http://www.w3.org/2000/svg" width="${product?120:24}" height="${product?100:24}" `)
    .replace(/class="[^"]*"\s*/g,'').replace(/aria-hidden="true"\s*/g,'')
    .replaceAll('currentColor',t.stroke).replaceAll('var(--bg-card-sub)',t.fill).replaceAll('var(--bg-card)',t.card)
    .replace(/var\(--product-accent,\s*#FF8500\)/g,t.accent)
    .replace(/stroke-width="[^"]+"/,`stroke-width="${product?1.45:1.65}"`);
}
const artContext={window:{}};vm.runInNewContext(fs.readFileSync(path.join(web,'product-art.js'),'utf8'),artContext);
const products=[['collar','collar','项圈'],['radio','chat','聊天'],['valve','water_valve','水阀'],['camera','camera','摄像头']];
for(const [key,name] of products){
  const source=artContext.window.EdgezProductArt[key];if(!source)throw new Error('Missing product '+key);
  exportAsset({id:'product_'+name,category:'products',name,svgByTheme:Object.fromEntries(Object.keys(themes).map(t=>[t,fixedSvg(source,t,'products')])),width:120,height:100,sourceKey:key});
}
const uiSource=fs.readFileSync(path.join(web,'product-ui.js'),'utf8');
const iconDefinition=uiSource.slice(uiSource.indexOf('  const I = '),uiSource.indexOf('  const action = '));
const iconContext={};vm.runInNewContext(iconDefinition+'\nthis.getIcon=I;',iconContext);
const names=[...iconDefinition.matchAll(/\b([a-z][a-zA-Z0-9]*):'/g)].map(m=>m[1]).filter(n=>!['collar','radio','valve','camera'].includes(n));
for(const name of [...new Set(names)])exportAsset({id:'ui_'+name,category:'ui',name,svgByTheme:Object.fromEntries(Object.keys(themes).map(t=>[t,fixedSvg(iconContext.getIcon(name),t,'ui')])),width:24,height:24,sourceKey:name});
const logo=fs.readFileSync(path.join(web,'assets/keepteen-logo.png'));
write('assets/brand/original/keepteen-logo-transparent.png',logo,{assetId:'keepteen_logo_original',category:'brand',format:'png',theme:'light',width:3651,height:1382,hasTransparency:true,sourceKey:'assets/keepteen-logo.png',note:'Original indexed PNG with transparency; preserved byte-for-byte.'});
const encoded=logo.toString('base64');
const darkFilter='<defs><filter id="dark-ink" color-interpolation-filters="sRGB"><feColorMatrix type="matrix" values="0 0 -1 0 1 -0.0039 -1 0.0039 0 1 -1 0 0 0 1 0 0 0 1 0"/></filter></defs>';
const logoSvg=theme=>`<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="156" height="59" viewBox="0 0 3651 1382">${theme==='dark'?darkFilter:''}<image width="3651" height="1382" xlink:href="data:image/png;base64,${encoded}"${theme==='dark'?' filter="url(#dark-ink)"':''}/></svg>`;
exportAsset({id:'keepteen_logo',category:'brand',name:'keepteen_logo',svgByTheme:{light:logoSvg('light'),dark:logoSvg('dark')},width:156,height:59,sourceKey:'assets/keepteen-logo.png'});
exportAsset({id:'edgez_logo',category:'brand',name:'edgez_logo',svgByTheme:Object.fromEntries(['light','dark'].map(theme=>[theme,fs.readFileSync(path.join(web,`assets/brand/edgez-logo-${theme}.svg`),'utf8')])),width:160,height:48,sourceKey:'assets/brand/edgez-logo-source.json'});
for(const name of ['edgez-logo-source.json','Inter-OFL.txt'])write('assets/brand/source/'+name,fs.readFileSync(path.join(web,'assets/brand',name)),{category:'brand',format:name.endsWith('.json')?'json':'text',sourceKey:'assets/brand/'+name});
const design={
  version:'2026.11.14',navigation:{rootTabs:['设备','设置'],settingsItems:['账号','HaLow','通知','通用','帮助','关于'],tabItemWidth:108,tabItemMinHeight:48,tabGapMax:28,chatEntry:'设备 → 聊天产品卡片',hideRootTabsInsideProducts:true},
  themes:{light:{canvas:'#F5F5F7',card:'#FFFFFF',surface:'#EFEFF2',text:'#1D1D1F',secondary:'#6E6E73',accent:'#FF8500',online:'#1479D7',danger:'#C52A24',productStroke:themes.light.stroke},dark:{canvas:'#000000',card:'#1C1C1E',surface:'#2C2C2E',text:'#F5F5F7',secondary:'#A1A1A6',accent:'#FF9A33',online:'#64ADF3',danger:'#FF6961',productStroke:themes.dark.stroke}},
  productArtwork:{viewBox:[0,0,120,100],strokeWidth:1.45,linecap:'round',linejoin:'round',webDisplaySize:[91,74],fit:'contain',pngBaseSize:[120,100],halo:{size:[67,51],opacity:0.65,colorToken:'surface',partOfImage:false}},
  uiIcons:{viewBox:[0,0,24,24],strokeWidth:1.65,typicalDisplaySize:[22,22],minimumTouchTarget:[44,44]},
  brand:{originalPng:[3651,1382],displaySize:[156,59],transparent:true,svgContainsEmbeddedRaster:true,dark:'White neutral ink; original orange retained. No white backing plate.'},
  layout:{cardRadius:23,sectionRadius:22,touchTargetMin:44,homeGridColumns:2,homeGridGap:12},
  sources:{'product-art.js':sha(fs.readFileSync(path.join(web,'product-art.js'))),'product-ui.js':sha(fs.readFileSync(path.join(web,'product-ui.js'))),'consumer-ui.css':sha(fs.readFileSync(path.join(web,'consumer-ui.css'))),'assets/brand/edgez-logo-light.svg':sha(fs.readFileSync(path.join(web,'assets/brand/edgez-logo-light.svg'))),'assets/brand/edgez-logo-dark.svg':sha(fs.readFileSync(path.join(web,'assets/brand/edgez-logo-dark.svg'))),'keepteen-logo.png':sha(logo)}
};
design.layout.homeAdd={touchTarget:44,visualFrame:36,icon:22,frameRadius:11,borderWidth:1};
design.navigation.home={title:'设备',areas:['全部','牧场','花园','仓库'],defaultArea:'全部',defaultProductCount:4,otherAreas:'empty display only',assignmentImplemented:false};
design.brand.joint={label:'KEEPTEEN × EdgeZ.ai',equalStanding:true,showHardwareSoftwareLabels:false,aboutShowsHelp:false,companyUrls:['https://www.keepteen.com/','https://www.edgez.ai/'],edgezSource:'assets/brand/source/edgez-logo-source.json'};
write('specs/DESIGN_TOKENS.json',JSON.stringify(design,null,2)+'\n',{format:'json',category:'specification'});
const manifestObject={version:'2026.11.14',sourceWeb:'reference/web_prototype',originalBrandSha256:sha(logo),products:products.map(([sourceKey,name,label])=>({assetId:'product_'+name,name,label,sourceKey})),uiIcons:[...new Set(names)],files:manifest};
fs.writeFileSync(path.join(root,'ASSET_MANIFEST.json'),JSON.stringify(manifestObject,null,2)+'\n');
// Remove only obsolete files recorded by the previous generated manifest, within this package.
const currentPaths=new Set(manifest.map(f=>f.path));
for(const item of previousManifest)if(!currentPaths.has(item.path)){
  const stale=path.resolve(root,item.path);if(!stale.startsWith(root+path.sep))throw new Error('Manifest path escaped package');
  if(fs.existsSync(stale))fs.unlinkSync(stale);
  const dir=path.dirname(stale);if(dir.endsWith('.imageset')&&fs.readdirSync(dir).length===0)fs.rmdirSync(dir);
}
const cards=(theme)=>products.map(([,name,label])=>`<article><div class="checker"><img src="assets/products/svg/${theme}/${name}.svg" alt="${label}" width="120" height="100"></div><h3>${label}</h3><a href="assets/products/svg/${theme}/${name}.svg">SVG</a> · <a href="assets/products/png/${theme}/${name}@3x.png">PNG @3x</a></article>`).join('');
const gallery=`<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>KEEPTEEN App 素材预览</title><style>*{box-sizing:border-box}body{font-family:system-ui,sans-serif;background:#eeeef2;color:#1d1d1f;max-width:1100px;margin:auto;padding:28px}h1{font-size:25px}p{line-height:1.8}section{padding:24px;margin:20px 0;border-radius:24px;background:#fff}.dark{background:#1c1c1e;color:#f5f5f7}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:18px}article{min-width:0}h3{font-size:15px}.checker{border-radius:14px;background-color:#f5f5f7;background-image:linear-gradient(45deg,#e0e0e4 25%,transparent 25%),linear-gradient(-45deg,#e0e0e4 25%,transparent 25%),linear-gradient(45deg,transparent 75%,#e0e0e4 75%),linear-gradient(-45deg,transparent 75%,#e0e0e4 75%);background-size:16px 16px;background-position:0 0,0 8px,8px -8px,-8px 0;padding:16px}.dark .checker{background-color:#202024;background-image:linear-gradient(45deg,#303034 25%,transparent 25%),linear-gradient(-45deg,#303034 25%,transparent 25%),linear-gradient(45deg,transparent 75%,#303034 75%),linear-gradient(-45deg,transparent 75%,#303034 75%)}a{color:inherit}img{display:block;max-width:100%;object-fit:contain}.brand{padding:20px 0}.brand img{width:234px;height:auto}.ui{display:flex;flex-wrap:wrap;gap:18px}.ui figure{margin:0;font-size:11px;text-align:center;width:70px}.ui img{margin:auto auto 8px;width:24px;height:24px}</style><h1>KEEPTEEN App 素材交接预览</h1><p>灰色棋盘仅用于核验透明背景，不在素材文件内。四种产品 SVG 来自当前正式网页；KEEPTEEN 原图保留且移除白色底板；EdgeZ.ai 为官网标识的中性矢量复现，来源元数据随包提供。<a href="README.md">先读交接说明</a> · <a href="reference/web_prototype/index.html">网页交互参考</a></p>${Object.keys(themes).map(theme=>`<section class="${theme}"><h2>${theme==='light'?'浅色界面素材':'深色界面素材'}</h2><div class="brand" style="display:flex;align-items:center;gap:18px;flex-wrap:wrap"><img src="assets/brand/png/${theme}/keepteen_logo@3x.png" alt="KEEPTEEN" style="width:180px"><span>×</span><img src="assets/brand/png/${theme}/edgez_logo@3x.png" alt="EdgeZ.ai" style="width:180px"></div><div class="grid">${cards(theme)}</div><h2>通用操作图标</h2><div class="ui">${[...new Set(names)].map(name=>`<figure><img src="assets/ui/svg/${theme}/${name}.svg" alt="${name}"><figcaption>${name}</figcaption></figure>`).join('')}</div></section>`).join('')}</html>`;
fs.writeFileSync(path.join(root,'ASSET_PREVIEW.html'),gallery);
console.log(JSON.stringify({products:products.length,uiIcons:new Set(names).size,themes:2,manifestFiles:manifest.length,output:root},null,2));
