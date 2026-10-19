package maclauncher.build;

import java.util.List;

/** Exact platform callbacks, shared by class-file and DEX build bindings. */
public final class UiHooks {
    public enum Position { ENTRY, RETURN }
    public record Member(String owner,String name,String descriptor) {}
    public record Field(String owner,String name,String descriptor) {}
    /** parameter includes the instance receiver; -1 is the original return value. */
    public record Value(int parameter,Field field,boolean isStatic) {
        static Value parameter(int index){return new Value(index,null,false);}
        static Value field(int index,String owner,String name,String descriptor){return new Value(index,new Field(owner,name,descriptor),false);}
        static Value field(String owner,String name,String descriptor){return new Value(0,new Field(owner,name,descriptor),true);}
    }
    public record Hook(Member source,Position position,Member target,List<Value> values) {}
    private static final String ROOT="android/view/ViewRootImpl",QUEUED=ROOT+"$QueuedInputEvent",WINDOW="maclauncher/android/framework/HostWindowManager";
    private static final Member SET_VIEW=new Member(ROOT,"setView","(Landroid/view/View;Landroid/view/WindowManager$LayoutParams;Landroid/view/View;I)V");
    public static final List<Hook> UI=List.of(
        new Hook(SET_VIEW,Position.ENTRY,new Member(WINDOW,"prepareView","(Landroid/view/ViewRootImpl;Landroid/view/WindowManager$LayoutParams;)V"),List.of(Value.parameter(0),Value.parameter(2))),
        new Hook(SET_VIEW,Position.RETURN,new Member(WINDOW,"viewAttached","(Landroid/view/ViewRootImpl;)V"),List.of(Value.parameter(0))),
        new Hook(new Member(ROOT,"dispatchDetachedFromWindow","()V"),Position.ENTRY,new Member(WINDOW,"viewDetached","(Landroid/view/ViewRootImpl;)V"),List.of(Value.parameter(0))),
        new Hook(new Member(ROOT,"finishInputEvent","(Landroid/view/ViewRootImpl$QueuedInputEvent;)V"),Position.ENTRY,new Member(WINDOW,"inputFinished","(Landroid/view/InputEvent;II)V"),List.of(
            Value.field(1,QUEUED,"mEvent","Landroid/view/InputEvent;"),Value.field(1,QUEUED,"mFlags","I"),Value.field(QUEUED,"FLAG_FINISHED_HANDLED","I"))),
        new Hook(new Member(ROOT,"getInputToken","()Landroid/os/IBinder;"),Position.RETURN,new Member("maclauncher/android/framework/HostInputManager","inputToken","(Landroid/view/ViewRootImpl;Landroid/os/IBinder;)Landroid/os/IBinder;"),List.of(Value.parameter(0),Value.parameter(-1)))
    );
    private UiHooks(){}
}
