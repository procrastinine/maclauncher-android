#include "webkit-platform.h"
#include "webkit-binding.h"

namespace klj::web {
namespace {
NSString* text(const Value&v){return v.type==Value::null?nil:[[NSString alloc]initWithCharacters:reinterpret_cast<const unichar*>(v.string.data()) length:v.string.size()];}
Value text(NSString*s){if(!s)return {};std::u16string v(s.length,0);[s getCharacters:reinterpret_cast<unichar*>(v.data()) range:NSMakeRange(0,s.length)];return Value(std::move(v));}
NSData* data(const Value&v){return v.type==Value::null?nil:[NSData dataWithBytes:v.bytesData() length:v.bytesSize()];}
Value data(NSData*v){return v?Value(std::vector<uint8_t>(static_cast<const uint8_t*>(v.bytes),static_cast<const uint8_t*>(v.bytes)+v.length)):Value();}
NSArray<NSString*>* strings(const Value&v){NSMutableArray*a=[NSMutableArray new];for(const auto&item:v.items)[a addObject:text(item)?:@""];return a;}
Value strings(NSArray<NSString*>*a){Args result;for(NSString*s in a)result.push_back(text(s));return Value(std::move(result));}
struct Client final:KljWebClient {
    std::shared_ptr<Callback> callback;void* parent;
    Client(std::shared_ptr<Callback> c,void*p):callback(std::move(c)),parent(p){}
    void* contentView()override{return parent;}
    void changed()override{callback->invoke(Event::changed,{});}
    void dimensions(int w,int h)override{callback->invoke(Event::dimensions,{w,h});}
    void navigation(NSString*s,NSString*u,NSString*t,int p)override{callback->invoke(Event::navigation,{text(s),text(u),text(t),p});}
    void history(NSArray<NSString*>*u,NSArray<NSString*>*t,int i)override{callback->invoke(Event::history,{strings(u),strings(t),i});}
    void dialog(int64_t i,NSString*k,NSString*u,NSString*m,NSString*v)override{callback->invoke(Event::dialog,{i,text(k),text(u),text(m),text(v)});}
    NSData* resource(NSString*u)override{return data(callback->invoke(Event::resource,{text(u)}));}
    void editor(NSString*j)override{callback->invoke(Event::editor,{text(j)});}
    void port(int64_t i,NSString*t)override{callback->invoke(Event::port,{i,text(t)});}
    bool navigationRequest(NSString*u,bool m,bool g,bool r,NSString*method,NSString*headers)override{return callback->invoke(Event::navigationRequest,{text(u),m,g,r,text(method),text(headers)}).number!=0;}
    NSString* interface(NSString*n,NSString*m,NSString*a)override{return text(callback->invoke(Event::interface,{text(n),text(m),text(a)}));}
    void script(int64_t r,NSString*j,NSString*e)override{callback->invoke(Event::script,{r,text(j),text(e)});}
    void frame(const void*p,int w,int h,NSString*e)override{callback->invoke(Event::frame,{p?Value::view(p,size_t(w)*h*4):Value(),w,h,text(e)});}
};
struct Result final:KljWebResult {
    std::shared_ptr<Callback> callback;explicit Result(std::shared_ptr<Callback>c):callback(std::move(c)){}
    void complete(NSString*j,NSString*e)override{if(callback)callback->invoke(Event::result,{text(j),text(e)});}
};
}
// Both in-process JNI and the ART presenter dispatch this complete operation
// family. The only content view is supplied by the existing window owner.
Value physicalRequest(std::shared_ptr<KljWebContext> context,Op op,const Args&a,std::shared_ptr<Callback>callback,void*parent){
    const auto n=[&](size_t i){return a.at(i).number;};
    const auto t=[&](size_t i){return text(a.at(i));};
    if(op!=Op::initialize&&op!=Op::create&&op!=Op::cookies&&op!=Op::cookieCommand&&op!=Op::storageCommand&&
       !klj_webkit_owns(context,n(0)))throw std::runtime_error("WebKit view does not belong to this application");
    switch(op){
    case Op::initialize:return text(klj_webkit_initialize(context,t(0),t(1),n(2)));
    case Op::create:return Value(klj_webkit_create(context,std::make_shared<Client>(std::move(callback),parent)));
    case Op::destroy:klj_webkit_destroy(n(0));break;
    case Op::visibility:klj_webkit_visibility(n(0),n(1));break;
    case Op::focus:klj_webkit_focus(n(0),n(1));break;
    case Op::size:klj_webkit_size(n(0),n(1),n(2),a.at(3).decimal);break;
    case Op::load:klj_webkit_load(n(0),t(1),strings(a.at(2)),strings(a.at(3)),data(a.at(4)));break;
    case Op::html:klj_webkit_html(n(0),t(1),t(2));break;
    case Op::script:klj_webkit_script(n(0),t(1),n(2));break;
    case Op::navigate:klj_webkit_navigate(n(0),n(1));break;
    case Op::reload:klj_webkit_reload(n(0));break;
    case Op::stop:klj_webkit_stop(n(0));break;
    case Op::snapshot:klj_webkit_snapshot(n(0));break;
    case Op::touch:klj_webkit_touch(n(0),n(1),a.at(2).decimal,a.at(3).decimal);break;
    case Op::key:klj_webkit_key(n(0),n(1),n(2),t(3),n(4));break;
    case Op::edit:klj_webkit_edit(n(0),n(1),t(2),n(3),n(4));break;
    case Op::editor:klj_webkit_editor(n(0),t(1),t(2),t(3));break;
    case Op::setting:klj_webkit_setting(n(0),t(1),t(2));break;
    case Op::interfaces:klj_webkit_interfaces(n(0),t(1));break;
    case Op::dialogResult:klj_webkit_dialogResult(n(0),n(1),n(2),t(3));break;
    case Op::state:return data(klj_webkit_state(n(0)));
    case Op::restore:return Value(klj_webkit_restore(n(0),data(a.at(1))));
    case Op::command:klj_webkit_command(n(0),t(1),t(2),n(3));break;
    case Op::cookies:return text(klj_webkit_cookies(context,t(0)));
    case Op::cookieCommand:klj_webkit_cookieCommand(context,t(0),t(1),std::make_shared<Result>(std::move(callback)));break;
    case Op::storageCommand:klj_webkit_storageCommand(context,t(0),t(1),std::make_shared<Result>(std::move(callback)));break;
    default:throw std::runtime_error("Invalid WebKit operation");
    }return {};
}
}
