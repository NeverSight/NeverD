package fixture;
public class Provider {
    public int calls;
    private final <T> Reply dispatch(Reply incoming, Cell<T> cell, Callback<? super T,Boolean> callback, boolean flag, float scale, long bits) {
        Trace.hit(1);Trace.receiver=this;calls++;
        Trace.flag=flag;Trace.scale=scale;Trace.bits=bits;
        T input=cell.read();Trace.input=input;
        Boolean value=callback.apply(input);Trace.booleanResult=value;Trace.hit(3);
        Trace.checked=value.booleanValue();
        return Trace.reply;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Reply entry(Provider owner, Reply incoming, Cell cell, Callback callback, boolean flag, float scale, long bits) {
        Trace.hit(6);Reply result=owner.dispatch(incoming,cell,callback,flag,scale,bits);Trace.hit(5);return result;
    }
}
