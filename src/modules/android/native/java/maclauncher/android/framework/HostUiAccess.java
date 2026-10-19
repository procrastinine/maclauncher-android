package maclauncher.android.framework;

import java.lang.reflect.*;

/** Access to Android's existing process/window owner records; no replacement framework state. */
final class HostUiAccess {
    private HostUiAccess(){}
    static Class<?> type(String name){try{return Class.forName(name,false,HostUiAccess.class.getClassLoader());}catch(ClassNotFoundException failure){throw new IllegalStateException("Android platform class is unavailable: "+name,failure);}}
    static Field field(Class<?> type,String name){try{var field=type.getDeclaredField(name);field.setAccessible(true);return field;}catch(ReflectiveOperationException failure){throw new IllegalStateException("Android platform field changed: "+type.getName()+"."+name,failure);}}
    static Object get(Object owner,Class<?> type,String name){try{return field(type,name).get(owner);}catch(IllegalAccessException failure){throw new IllegalStateException(failure);}}
    static void set(Object owner,Class<?> type,String name,Object value){try{field(type,name).set(owner,value);}catch(IllegalAccessException failure){throw new IllegalStateException(failure);}}
    static Method method(Class<?> type,String name,Class<?>...arguments){try{var method=type.getDeclaredMethod(name,arguments);method.setAccessible(true);return method;}catch(ReflectiveOperationException failure){throw new IllegalStateException("Android platform method changed: "+type.getName()+"."+name,failure);}}
    static Object call(Method method,Object owner,Object...arguments){try{return method.invoke(owner,arguments);}catch(InvocationTargetException failure){return raise(failure.getCause());}catch(ReflectiveOperationException failure){throw new IllegalStateException(failure);}}
    static Object construct(Class<?> type,Class<?>[] arguments,Object...values){try{var constructor=type.getDeclaredConstructor(arguments);constructor.setAccessible(true);return constructor.newInstance(values);}catch(InvocationTargetException failure){return raise(failure.getCause());}catch(ReflectiveOperationException failure){throw new IllegalStateException("Android platform constructor changed: "+type.getName(),failure);}}
    @SuppressWarnings("unchecked")static <T,E extends Throwable>T raise(Throwable failure)throws E{throw(E)failure;}
}
