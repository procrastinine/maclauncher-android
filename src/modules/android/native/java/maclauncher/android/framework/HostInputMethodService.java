package maclauncher.android.framework;

import android.content.Context;
import android.os.*;
import android.view.inputmethod.*;
import android.window.ImeOnBackInvokedDispatcher;
import com.android.internal.inputmethod.*;
import com.android.internal.view.IInputMethodManager;
import java.util.*;

/** Native text input endpoint beneath the original Android input connection lifecycle. */
public final class HostInputMethodService extends IInputMethodManager.Default {
    private static final HostInputMethodService SERVICE=new HostInputMethodService();
    private static boolean installed;
    private final Map<IInputMethodClient,Integer> clients=new WeakHashMap<>();
    private HostInputMethodService(){}
    public static synchronized void install(){if(!installed){HostServices.register(Context.INPUT_METHOD_SERVICE,SERVICE,"com.android.internal.view.IInputMethodManager");installed=true;}}
    @Override public synchronized void addClient(IInputMethodClient client,IRemoteInputConnection fallback,int display){Objects.requireNonNull(client);if(display!=android.view.Display.DEFAULT_DISPLAY)throw new IllegalArgumentException("Display is not owned by this process");clients.put(client,display);}
    @Override public InputMethodInfoSafeList getInputMethodList(int user,int flags){return InputMethodInfoSafeList.empty();}
    @Override public InputMethodInfoSafeList getEnabledInputMethodList(int user){return InputMethodInfoSafeList.empty();}
    @Override public List<InputMethodInfo> getInputMethodListLegacy(int user,int flags){return List.of();}
    @Override public List<InputMethodInfo> getEnabledInputMethodListLegacy(int user){return List.of();}
    @Override public List<InputMethodSubtype> getEnabledInputMethodSubtypeList(String id,boolean implicit,int user){return List.of();}
    @Override public boolean showSoftInput(IInputMethodClient client,IBinder window,ImeTracker.Token tracker,int flags,int tool,ResultReceiver receiver,int reason,boolean asynchronous){if(receiver!=null)receiver.send(InputMethodManager.RESULT_UNCHANGED_HIDDEN,null);return false;}
    @Override public boolean hideSoftInput(IInputMethodClient client,IBinder window,ImeTracker.Token tracker,int flags,ResultReceiver receiver,int reason,boolean asynchronous){if(receiver!=null)receiver.send(InputMethodManager.RESULT_UNCHANGED_HIDDEN,null);return false;}
    @Override public InputBindResult startInputOrWindowGainedFocus(int reason,IInputMethodClient client,IBinder window,int startFlags,int softMode,int windowFlags,EditorInfo editor,IRemoteInputConnection connection,IRemoteAccessibilityInputConnection accessibility,int sdk,int user,ImeOnBackInvokedDispatcher back,boolean async){HostTextInput.connect(connection,editor);return InputBindResult.NO_IME;}
    @Override public void startInputOrWindowGainedFocusAsync(int reason,IInputMethodClient client,IBinder window,int startFlags,int softMode,int windowFlags,EditorInfo editor,IRemoteInputConnection connection,IRemoteAccessibilityInputConnection accessibility,int sdk,int user,ImeOnBackInvokedDispatcher back,boolean async,int sequence,boolean notify)throws RemoteException{HostTextInput.connect(connection,editor);Objects.requireNonNull(client).onStartInputResult(InputBindResult.NO_IME,sequence);}
    @Override public void acceptStylusHandwritingDelegationAsync(IInputMethodClient client,int user,String delegate,String delegator,int flags,IBooleanListener listener)throws RemoteException{Objects.requireNonNull(listener).onResult(false);}
    @Override public void startConnectionlessStylusHandwriting(IInputMethodClient client,int user,CursorAnchorInfo anchor,String delegate,String delegator,IConnectionlessHandwritingCallback callback)throws RemoteException{Objects.requireNonNull(callback).onError(ConnectionlessHandwritingCallback.CONNECTIONLESS_HANDWRITING_ERROR_UNSUPPORTED);}
    public static void shutdown(){synchronized(SERVICE){SERVICE.clients.clear();HostTextInput.shutdown();}}
}
