import fs from 'node:fs';
import path from 'node:path';

// Darwin supplies the original software KeyMint implementation as the default
// device. Device discovery and stored metadata must use its actual hardware
// info; requesting Android's default slot must not manufacture a TEE claim.
export function adaptSoftwareProvider(root) {
function adapt(file,changes){let text=fs.readFileSync(root+'/'+file,'utf8');for(const [before,after] of changes){if(!text.includes(before))throw Error('Original provider seam changed: '+file+' '+before);text=text.replace(before,after);}fs.writeFileSync(root+'/'+file,text);}
adapt('globals.rs',[
 ['let uuid: Uuid = sec_level.into();','let actual_level = hw_info.securityLevel;\n        let uuid: Uuid = actual_level.into();'],
 ['self.uuid_by_sec_level.insert(sec_level, uuid);','self.uuid_by_sec_level.insert(sec_level, uuid);\n        self.uuid_by_sec_level.insert(actual_level, uuid);'],
 ['        SecurityLevel::TRUSTED_ENVIRONMENT => {','        SecurityLevel::TRUSTED_ENVIRONMENT | SecurityLevel::SOFTWARE => {'],
]);
adapt('security_level.rs',[
 ['        let result = BnKeystoreSecurityLevel::new_binder(','        let security_level = hw_info.securityLevel;\n        let result = BnKeystoreSecurityLevel::new_binder('],
]);
adapt('service.rs',[
 ['        result.uuid_by_sec_level.insert(SecurityLevel::TRUSTED_ENVIRONMENT, uuid);','        let (_, default_info, _) = crate::globals::get_keymint_device(&SecurityLevel::TRUSTED_ENVIRONMENT)?;\n        result.uuid_by_sec_level.insert(default_info.securityLevel, uuid);'],
 ['        if let Some(dev) = self\n            .uuid_by_sec_level\n            .get(&sec_level)','        // Android clients request the default slot as TRUSTED_ENVIRONMENT.\n        // The real software provider remains SOFTWARE in every returned key\n        // and authorization record. Explicit STRONGBOX never uses this route.\n        let sec_level = if sec_level == SecurityLevel::TRUSTED_ENVIRONMENT\n            && !self.uuid_by_sec_level.contains_key(&sec_level)\n            && self.uuid_by_sec_level.contains_key(&SecurityLevel::SOFTWARE) {\n            SecurityLevel::SOFTWARE\n        } else { sec_level };\n        if let Some(dev) = self\n            .uuid_by_sec_level\n            .get(&sec_level)'],
]);
}
