#pragma once
#include <sys/types.h>
#include <algorithm>
#include <deque>
#include <set>

// Authorization is produced only by the launcher's hardware observer. Guest
// request timing cannot create a gesture; a captured gesture can be used once.
class KLClipboardGestures {
  struct Gesture {pid_t pid;bool copy;double time;};
  std::set<pid_t> enrolled;
  std::deque<Gesture> gestures;
  pid_t previous=0;
  bool copyDown=false,pasteDown=false;
  static constexpr double lifetime=2.;
  void expire(double now){while(!gestures.empty()&&now-gestures.front().time>lifetime)gestures.pop_front();}
public:
  void enroll(pid_t pid){if(pid>0)enrolled.insert(pid);}
  void retire(pid_t pid){enrolled.erase(pid);std::erase_if(gestures,[&](auto&g){return g.pid==pid;});if(previous==pid){previous=0;copyDown=pasteDown=false;}}
  bool contains(pid_t pid)const{return enrolled.contains(pid);}
  void sample(pid_t foreground,bool command,bool otherModifier,bool copy,bool paste,double now){
    expire(now);
    if(!contains(foreground))foreground=0;
    // An already-held key while switching apps is not a new shortcut.
    if(previous!=foreground){previous=foreground;copyDown=copy;pasteDown=paste;return;}
    if(foreground&&command&&!otherModifier){
      if(copy&&!copyDown)gestures.push_back({foreground,true,now});
      if(paste&&!pasteDown)gestures.push_back({foreground,false,now});
      while(gestures.size()>8)gestures.pop_front();
    }
    copyDown=copy;pasteDown=paste;
  }
  bool consume(pid_t expected,pid_t foreground,bool copy,double now){
    expire(now);if(expected<=0||expected!=foreground||!contains(expected))return false;
    auto found=std::find_if(gestures.begin(),gestures.end(),[&](auto&g){return g.pid==expected&&g.copy==copy;});
    if(found==gestures.end())return false;gestures.erase(found);return true;
  }
};
