package fixture;
public final class Child extends Middle {
    public Child() { super(); }
    @SuppressWarnings("unchecked")
    public <T> T[] collect(int left,int right,Class<T> token) {
        Trace.hit(1,this,token,left,right,null);
        Object[] result;
        if(token==null)result=super.collect(left,right,String.class);
        else result=super.collect(left,right,token);
        Trace.hit(4,this,token,left,right,result);
        return (T[])result;
    }
    @SuppressWarnings("unchecked")
    public Object run(Object input,int left,int right) {
        Class token=(Class)input;
        Trace.hit(0,this,token,left,right,null);
        Object[] result=collect(left,right,token);
        Trace.hit(5,this,token,left,right,result);
        return (String[])result;
    }
    @SuppressWarnings("unchecked")
    public void ignore(Object input,int left,int right) {
        Class token=(Class)input;
        Trace.hit(0,this,token,left,right,null);
        collect(left,right,token);
        Trace.hit(5,this,token,left,right,null);
    }
}
