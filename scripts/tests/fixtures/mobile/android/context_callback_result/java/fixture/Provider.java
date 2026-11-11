package fixture;
public final class Provider {
    private interface Callback<T> {
        void accept(CharSequence context,int left,int right,int limit,boolean flag);
        T getResult();
    }
    public static class Adapter implements Callback<Object> {
        public Object reply;
        public Adapter() {}
        public void accept(CharSequence context,int left,int right,int limit,boolean flag) {}
        public Object getResult() {return reply;}
    }
    public Provider() {}
    private <T> T read(CharSequence context,int left,int right,int limit,boolean flag,Callback<T> callback) {
        Trace.hit(1,this,context,callback,left,right,limit,flag,null);
        callback.accept(context,left,right,limit,flag);
        T result=callback.getResult();
        Trace.hit(3,this,context,callback,left,right,limit,flag,result);
        return result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public String run(Object text,Object input,int left,int right,int limit,boolean flag) {
        CharSequence context=(CharSequence)text;
        Adapter callback=(Adapter)input;
        Trace.hit(0,this,context,callback,left,right,limit,flag,null);
        Object result=read(context,left,right,limit,flag,(Callback)callback);
        Trace.hit(4,this,context,callback,left,right,limit,flag,result);
        return (String)result;
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public void ignore(Object text,Object input,int left,int right,int limit,boolean flag) {
        CharSequence context=(CharSequence)text;
        Adapter callback=(Adapter)input;
        Trace.hit(0,this,context,callback,left,right,limit,flag,null);
        read(context,left,right,limit,flag,(Callback)callback);
        Trace.hit(4,this,context,callback,left,right,limit,flag,null);
    }
}
