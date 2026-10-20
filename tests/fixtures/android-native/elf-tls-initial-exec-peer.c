extern __thread int ie_initialized;
extern __thread unsigned char ie_zero[73];
extern __thread int tls_dependency;
int *peer_address(void){return &ie_initialized;}
unsigned char *peer_zero_address(void){return ie_zero;}
int *peer_dependency_address(void){return &tls_dependency;}
