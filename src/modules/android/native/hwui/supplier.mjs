import fs from 'node:fs';
import path from 'node:path';
/** Optional complete HWUI producer. Omission retains the original image file. */
export async function buildHwuiSupplier({hwui,output},build) {
 if(!hwui)return {};
 const {licenses,...options}=hwui;
 build??=(await import('./build.mjs')).buildHwui;
 const product=await build({...options,...(licenses?{preparedLicenses:licenses}:{}),work:options.work||path.join(output,'build/hwui')});
 if(!product?.path||!fs.statSync(product.path).isFile())throw Error('Complete HWUI producer returned no library');
 const licenseRoot=licenses||product.licenses;
 return {hwuiLibrary:product.path,hwuiLicenses:licenseRoot};
}
