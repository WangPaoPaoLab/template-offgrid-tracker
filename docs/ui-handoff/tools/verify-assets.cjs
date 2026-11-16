const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..'),manifest=JSON.parse(fs.readFileSync(path.join(root,'ASSET_MANIFEST.json'),'utf8'));
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const files=new Set();let png=0,svg=0,ios=0;
for(const item of manifest.files){
  assert(!files.has(item.path),'Duplicate path '+item.path);files.add(item.path);
  const file=path.resolve(root,item.path);assert(file.startsWith(root+path.sep),'Path escapes package');
  const bytes=fs.readFileSync(file);assert.equal(hash(bytes),item.sha256,item.path+' integrity');
  if(item.format==='png'){
    assert.equal(bytes.subarray(1,4).toString(),'PNG');assert.equal(bytes.readUInt32BE(16),item.width);assert.equal(bytes.readUInt32BE(20),item.height);
    assert(bytes[25]===6||bytes.includes(Buffer.from('tRNS')),'PNG needs alpha channel or transparency '+item.path);png++;
  }
  if(item.format==='svg'){
    const text=bytes.toString('utf8');assert(text.includes('xmlns="http://www.w3.org/2000/svg"'));
    assert(!text.includes('var(')&&!text.includes('currentColor'),'SVG must work without external CSS '+item.path);
    for(const m of text.matchAll(/(?:xlink:)?href="([^"]+)"/g))assert(m[1].startsWith('data:')||m[1].startsWith('#'),'No external SVG dependencies '+item.path);
    svg++;
  }
  if(item.platform==='ios'&&item.format==='json'){
    const contents=JSON.parse(bytes);assert.equal(contents.images.length,6);assert.equal(contents.images.filter(v=>v.appearances).length,3);
    for(const image of contents.images)assert(fs.existsSync(path.join(path.dirname(file),image.filename)));ios++;
  }
}
assert.equal(manifest.products.map(p=>p.assetId).join(','),'product_collar,product_chat,product_water_valve,product_camera');
assert.equal(manifest.products.length,4);assert.equal(manifest.uiIcons.length,23);
const original=fs.readFileSync(path.join(root,'assets/brand/original/keepteen-logo-transparent.png'));
assert.equal(hash(original),manifest.originalBrandSha256);
const web=path.join(root,'reference/web_prototype');
const tokens=JSON.parse(fs.readFileSync(path.join(root,'specs/DESIGN_TOKENS.json'),'utf8'));
for(const [name,digest] of Object.entries(tokens.sources))assert.equal(hash(fs.readFileSync(path.join(web,name==='keepteen-logo.png'?'assets/'+name:name))),digest,'Reference source '+name);
assert.deepEqual(tokens.navigation.rootTabs,['设备','设置']);assert(tokens.navigation.hideRootTabsInsideProducts);
assert(fs.existsSync(path.join(root,'reference/00_USER_REQUIREMENTS.md')));
assert(fs.existsSync(path.join(root,'reference/backend_alignment/MAP_GEOFENCE_IMPLEMENTATION_HANDOFF.md')));
for(const density of ['mdpi','hdpi','xhdpi','xxhdpi','xxxhdpi'])for(const night of ['', 'night-']){
  const folder=path.join(root,'platforms/android/drawable-'+night+density);assert.equal(fs.readdirSync(folder).filter(f=>f.endsWith('.png')).length,29);
}
console.log(JSON.stringify({verifiedFiles:files.size,pngFiles:png,svgFiles:svg,iosImageSets:ios,androidDensityFolders:10,products:4,uiIcons:23,rootTabs:tokens.navigation.rootTabs},null,2));
