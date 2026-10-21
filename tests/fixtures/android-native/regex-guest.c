#include <regex.h>
#include <stddef.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
_Static_assert(sizeof(regex_t)==32,"original Android LP64 regex record");
_Static_assert(offsetof(regex_t,re_nsub)==8&&offsetof(regex_t,re_endp)==16&&offsetof(regex_t,re_g)==24,"original Android regex fields");
_Static_assert(sizeof(regmatch_t)==16&&REG_GNU==0400,"original Android offsets and GNU flag");
int regex_probe(void){
 struct {unsigned long before;regex_t re;unsigned long after;} owner={0x1020304050607080ul,{0},0x8877665544332211ul};
 regex_t *re=&owner.re;regmatch_t m[5];
 CHECK(regcomp(re,"(ab)+([0-9]{2})",REG_EXTENDED)==0&&re->re_nsub==2);
 CHECK(regexec(re,"--abab42!",5,m,0)==0);
 CHECK(m[0].rm_so==2&&m[0].rm_eo==8&&m[1].rm_so==4&&m[1].rm_eo==6&&m[2].rm_so==6&&m[2].rm_eo==8&&m[3].rm_so==-1&&m[3].rm_eo==-1);
 CHECK(regexec(re,"ab4!",5,m,0)==REG_NOMATCH);regfree(re);CHECK(re->re_magic==0);regfree(re);
 CHECK(regcomp(re,"^\\(ab\\)\\1$",REG_BASIC)==0&&re->re_nsub==1);
 CHECK(regexec(re,"abab",2,m,0)==0&&m[1].rm_so==0&&m[1].rm_eo==2);
 CHECK(regexec(re,"abac",2,m,0)==REG_NOMATCH);regfree(re);
 CHECK(regcomp(re,"^a\\+\\(b\\|c\\)\\?$",REG_BASIC|REG_GNU)==0);
 CHECK(regexec(re,"aaab",0,NULL,0)==0&&regexec(re,"aaa",0,NULL,0)==0&&regexec(re,"aaad",0,NULL,0)==REG_NOMATCH);regfree(re);
 CHECK(regcomp(re,"^\\w+\\s+\\W$",REG_EXTENDED|REG_GNU)==0);
 CHECK(regexec(re,"Ab9 \t!",0,NULL,0)==0&&regexec(re,"Ab9 \tX",0,NULL,0)==REG_NOMATCH);regfree(re);
 CHECK(regcomp(re,"\\bword\\b",REG_EXTENDED|REG_GNU)==0);
 CHECK(regexec(re," word!",1,m,0)==0&&m[0].rm_so==1&&m[0].rm_eo==5);
 CHECK(regexec(re,"sword",0,NULL,0)==REG_NOMATCH);regfree(re);
 CHECK(regcomp(re,"(x|)",REG_EXTENDED|REG_GNU)==REG_EMPTY);regfree(re);
 CHECK(regcomp(re,"^[[:alpha:]]+$",REG_EXTENDED|REG_ICASE)==0);
 CHECK(regexec(re,"aBc",0,NULL,0)==0&&regexec(re,"\351",0,NULL,0)==REG_NOMATCH);regfree(re);
 CHECK(regcomp(re,"\351+",REG_EXTENDED)==0&&regexec(re,"x\351\351",1,m,0)==0&&m[0].rm_so==1&&m[0].rm_eo==3);regfree(re);
 CHECK(regcomp(re,"^b$",REG_EXTENDED|REG_NEWLINE)==0&&regexec(re,"a\nb\nc",1,m,0)==0&&m[0].rm_so==2&&m[0].rm_eo==3);regfree(re);
 CHECK(regcomp(re,"^ab$",REG_EXTENDED)==0&&regexec(re,"ab",0,NULL,REG_NOTBOL)==REG_NOMATCH&&regexec(re,"ab",0,NULL,REG_NOTEOL)==REG_NOMATCH);regfree(re);
 CHECK(regcomp(re,"a+b",REG_NOSPEC)==0&&regexec(re,"a+b",0,NULL,0)==0&&regexec(re,"aab",0,NULL,0)==REG_NOMATCH);regfree(re);
 const char pattern[]="ab+unused";re->re_endp=pattern+3;
 CHECK(regcomp(re,pattern,REG_EXTENDED|REG_PEND)==0&&regexec(re,"abbb",1,m,0)==0&&m[0].rm_eo==4);regfree(re);
 const char bounded[]={'x','a','\0','b','y'};CHECK(regcomp(re,"b",REG_EXTENDED)==0);m[0]=(regmatch_t){3,4};
 CHECK(regexec(re,bounded,1,m,REG_STARTEND)==0&&m[0].rm_so==3&&m[0].rm_eo==4);
 m[0]=(regmatch_t){0,3};CHECK(regexec(re,bounded,1,m,REG_STARTEND)==REG_NOMATCH);
 m[0]=(regmatch_t){4,3};CHECK(regexec(re,bounded,1,m,REG_STARTEND)==REG_INVARG);regfree(re);
 CHECK(regcomp(re,"(a)",REG_EXTENDED|REG_NOSUB)==0);m[0]=(regmatch_t){71,72};CHECK(regexec(re,"a",1,m,0)==0&&m[0].rm_so==71&&m[0].rm_eo==72);regfree(re);
 CHECK(regcomp(re,"[",REG_EXTENDED)==REG_EBRACK);regfree(re);
 CHECK(regcomp(re,"\\1",REG_BASIC)==REG_ESUBREG);regfree(re);
 CHECK(regcomp(re,"a",REG_EXTENDED|REG_NOSPEC)==REG_INVARG);
 char full[128],tiny[3]={'X','X','Q'};size_t size=regerror(REG_EBRACK,NULL,NULL,0);
 CHECK(size>3&&size<=sizeof full&&regerror(REG_EBRACK,NULL,full,sizeof full)==size&&strlen(full)+1==size);
 CHECK(regerror(REG_EBRACK,NULL,tiny,2)==size&&tiny[1]=='\0'&&tiny[2]=='Q');
 CHECK(regerror(REG_EBRACK|REG_ITOA,NULL,full,sizeof full)==sizeof("REG_EBRACK")&&!strcmp(full,"REG_EBRACK"));
 re->re_endp="REG_EBRACK";CHECK(regerror(REG_ATOI,re,full,sizeof full)==2&&!strcmp(full,"7"));
 for(int i=0;i<256;i++){CHECK(regcomp(re,"([ab]+|[0-9]{2,4})",REG_EXTENDED)==0&&regexec(re,"123",2,m,0)==0);regfree(re);}
 CHECK(owner.before==0x1020304050607080ul&&owner.after==0x8877665544332211ul);
 return 0;
}
