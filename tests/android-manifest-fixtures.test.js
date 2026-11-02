const test=require('node:test'),assert=require('node:assert/strict');
const {binaryXml}=require('./helpers/binary-xml');
const {parseBinaryManifest}=require('../src/modules/android/binary-manifest');
test('text fixtures retain provider authority declarations and complete bootstrap permissions',()=>{
 const provider=require('./fixtures/android-framework-services/manifests/providers.xml.tree.json');
 const metadata=parseBinaryManifest(binaryXml(provider.tree,true,provider.resourceIds));
 assert.equal(metadata.packageName,'owned.providers');
 assert.equal(metadata.applicationProviders.length,4);
 const bootstrap=require('./fixtures/android-activity-bootstrap/AndroidManifest.xml.tree.json');
 const permissions=parseBinaryManifest(binaryXml(bootstrap.tree,true,bootstrap.resourceIds)).permissions;
 assert.deepEqual(permissions,['android.permission.INTERNET','android.permission.POST_NOTIFICATIONS','android.permission.ACCESS_FINE_LOCATION','android.permission.CAMERA']);
});
