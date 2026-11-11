import fixture.*;
public final class ClosedSuperArgumentsCaller {
    public static Child build(Provider p,Fn<Object,Unit> a,Fn<Object,Unit> b,Object token) { return new Child(p,a,b,token); }
    public static Unit call(Parent value,Object input,boolean write) { return value.call(input,write); }
}
