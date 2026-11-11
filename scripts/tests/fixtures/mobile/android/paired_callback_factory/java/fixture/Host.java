package fixture;
public class Host {
    public Host() {}
    private <I,O> Box<I> make(Pair<I,O> input,Function<Void,Registry> context,Callback<O> callback) {
        Trace.hit(1,this,input,context,callback,null);
        Registry registry=context.apply(null);
        O value=input.output;
        callback.accept(value);
        Box<I> result=input.box;
        Trace.hit(3,this,input,context,callback,result);
        return result;
    }
    @SuppressWarnings("unchecked")
    public Object run(Object pair,Object function,Object consumer) {
        Pair input=(Pair)pair;Function context=(Function)function;Callback callback=(Callback)consumer;
        Trace.hit(0,this,input,context,callback,null);
        Box result=make(input,context,callback);
        Trace.hit(4,this,input,context,callback,result);
        return (String)result.value;
    }
    @SuppressWarnings("unchecked")
    public void ignore(Object pair,Object function,Object consumer) {
        Pair input=(Pair)pair;Function context=(Function)function;Callback callback=(Callback)consumer;
        Trace.hit(0,this,input,context,callback,null);
        make(input,context,callback);
        Trace.hit(4,this,input,context,callback,null);
    }
}
