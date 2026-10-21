import fs from 'node:fs';
import path from 'node:path';
/** Optional complete HWUI producer. Omission retains the original image file. */
export async function buildHwuiSupplier({hwui,output},build) {
 if(!hwui)return {};
 if(hwui.notices&&(typeof hwui.notices!=='string'||!path.isAbsolute(hwui.notices)||!fs.statSync(hwui.notices).isDirectory()))throw Error('Complete HWUI module notices must be a directory');
 const {notices,...options}=hwui;
 build??=(await import('./build.mjs')).buildHwui;
 const product=await build({...options,...(notices?{preparedNotices:notices}:{}),work:options.work||path.join(output,'build/hwui')});
 if(!product?.path||!fs.statSync(product.path).isFile())throw Error('Complete HWUI producer returned no library');
 const noticeRoot=notices||product.notices;if(typeof noticeRoot!=='string'||!path.isAbsolute(noticeRoot)||!fs.statSync(noticeRoot).isDirectory())throw Error('Complete HWUI producer returned no notices');
 return {hwuiLibrary:product.path,hwuiNotices:noticeRoot};
}
