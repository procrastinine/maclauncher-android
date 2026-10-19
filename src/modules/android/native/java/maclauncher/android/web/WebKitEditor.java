package maclauncher.android.web;

import android.os.Handler;
import android.text.*;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;

/** The document supplies an Editable; Android owns its editing algorithms. */
final class WebKitEditor extends BaseInputConnection implements TextWatcher,SpanWatcher {
    record State(String text,int start,int end,int composingStart,int composingEnd) {
        static State read(Editable value){return new State(value.toString(),Selection.getSelectionStart(value),Selection.getSelectionEnd(value),getComposingSpanStart(value),getComposingSpanEnd(value));}
    }
    interface Target {
        void update(State before,State after,long revision);
        boolean action(WebKitEditor editor,int action);
        boolean key(KeyEvent event);
    }
    private final Editable content=Editable.Factory.getInstance().newEditable("");
    private final Handler handler;
    private final Target target;
    private final Runnable publication=this::publish;
    private State published;
    private long revision;
    private int batches;
    private boolean synchronizing,retired,dirty,scheduled;

    WebKitEditor(View view,Handler handler,Target target,State state,long revision){
        super(view,true);this.handler=handler;this.target=target;
        synchronize(state,revision);
        content.setSpan(this,0,content.length(),Spanned.SPAN_INCLUSIVE_INCLUSIVE);
    }
    /** This is BaseInputConnection's documented real-editor extension point. */
    @Override public Editable getEditable(){return retired?null:content;}
    @Override public boolean beginBatchEdit(){if(retired)return false;batches++;return true;}
    @Override public boolean endBatchEdit(){if(retired||batches==0)return false;if(--batches==0)publish();return batches>0;}
    @Override public boolean sendKeyEvent(KeyEvent event){return !retired&&target.key(event);}
    @Override public boolean performContextMenuAction(int action){return !retired&&target.action(this,action);}

    void synchronize(State state,long observedRevision){
        if(retired||observedRevision<revision)return;
        synchronizing=true;
        try {
            if(!content.toString().equals(state.text()))content.replace(0,content.length(),state.text());
            removeComposingSpans(content);
            if(state.composingStart()>=0&&state.composingEnd()>=state.composingStart()&&state.composingEnd()<=content.length())
                setComposingSpans(content,state.composingStart(),state.composingEnd());
            if(state.start()>=0&&state.end()>=0&&state.start()<=content.length()&&state.end()<=content.length())Selection.setSelection(content,state.start(),state.end());
            else Selection.removeSelection(content);
            published=State.read(content);revision=observedRevision;dirty=false;
        }finally{synchronizing=false;}
    }
    void retire(){retired=true;handler.removeCallbacks(publication);content.removeSpan(this);}
    private void changed(){
        if(synchronizing||retired)return;dirty=true;
        if(batches==0&&!scheduled){scheduled=true;handler.post(publication);}
    }
    private void publish(){
        scheduled=false;handler.removeCallbacks(publication);
        if(retired||synchronizing||batches>0||!dirty)return;
        dirty=false;State next=State.read(content);
        if(next.equals(published))return;
        State previous=published;published=next;target.update(previous,next,++revision);
    }
    @Override public void beforeTextChanged(CharSequence text,int start,int count,int after){}
    @Override public void onTextChanged(CharSequence text,int start,int before,int count){}
    @Override public void afterTextChanged(Editable text){changed();}
    @Override public void onSpanAdded(Spannable text,Object what,int start,int end){if(what!=this)changed();}
    @Override public void onSpanRemoved(Spannable text,Object what,int start,int end){if(what!=this)changed();}
    @Override public void onSpanChanged(Spannable text,Object what,int oldStart,int oldEnd,int newStart,int newEnd){if(what!=this)changed();}
}
