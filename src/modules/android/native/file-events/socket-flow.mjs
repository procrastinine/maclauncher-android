/** Darwin write readiness reports current capacity, including a notification
 * queued between the readiness transition and kevent collection. */
export function adaptFileEventSocketFlow(source){
 const before='                    assert (received[i].data >= wrk->sockbufsize);';
 const after=`                    /* A ready socket can still contain earlier whole events.
                     * Flush only the reported capacity; reset coalescing after
                     * the communication buffer has actually drained. */
                    if (received[i].data < wrk->sockbufsize) {
                        sbspace = received[i].data > 0 ? (size_t)received[i].data : 0;
                        continue;
                    }`;
 if(source.split(before).length!==2)throw Error('File event socket readiness boundary changed');
 return source.replace(before,after);
}
