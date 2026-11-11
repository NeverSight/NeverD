import fixture.Client;
import fixture.Provider;
public final class ArrayObjectScalarCaller {
    public static int direct(Object[] values,Object key) {return Provider.<Object>score(values,key);}
    public static int lookup(Object[] values,Object key,boolean tail) {return new Client<Object>(values,tail).lookup(key);}
}
