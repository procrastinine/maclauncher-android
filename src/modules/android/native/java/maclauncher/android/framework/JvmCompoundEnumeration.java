package maclauncher.android.framework;
import java.util.*;
/** JDK equivalent of the removed sun.misc enumeration combiner. */
public final class JvmCompoundEnumeration<E> implements Enumeration<E> {
    private final Iterator<Enumeration<E>> sources;
    private Enumeration<E> current=Collections.emptyEnumeration();
    public JvmCompoundEnumeration(Enumeration<E>[] sources){this.sources=Arrays.asList(sources).iterator();}
    public boolean hasMoreElements(){while(!current.hasMoreElements()&&sources.hasNext()){current=sources.next();if(current==null)current=Collections.emptyEnumeration();}return current.hasMoreElements();}
    public E nextElement(){if(!hasMoreElements())throw new NoSuchElementException();return current.nextElement();}
}
