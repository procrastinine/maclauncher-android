__thread unsigned char oversized_tls[1024*1024+1];
unsigned char *oversized_address(void){return oversized_tls;}
