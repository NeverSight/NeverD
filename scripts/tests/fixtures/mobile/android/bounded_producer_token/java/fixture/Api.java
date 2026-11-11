package fixture;
public final class Api {
    private Api() {}
    @SuppressWarnings("unchecked")
    public static <T extends Bound> Result<T> build(Context context,Token<T> token,Producer<? extends Store> first,Producer<? extends Extras> second,Producer<? extends Context.Factory> third) {
        Trace.hit(1,context,token,first,second,third,null);
        if(context.eager) {
            Store store=first.get();Extras extras=second.get();Context.Factory factory=third.get();
            Trace.hit(2,context,store,extras,factory,token,null);
        }
        Result<T> result=(Result<T>)context.reply;
        Trace.hit(3,context,token,first,second,third,result);
        return result;
    }
    @SuppressWarnings("unchecked")
    public static Object run(Object input,Object type,Object a,Object b,Object d) {
        Context context=(Context)input;Token token=(Token)type;Producer first=(Producer)a,second=(Producer)b,third=(Producer)d;
        Trace.hit(0,context,token,first,second,third,null);
        Result result=build(context,token,first,second,third);
        Trace.hit(4,context,token,first,second,third,result);
        return (String)result.value;
    }
    @SuppressWarnings("unchecked")
    public static void ignore(Object input,Object type,Object a,Object b,Object d) {
        Context context=(Context)input;Token token=(Token)type;Producer first=(Producer)a,second=(Producer)b,third=(Producer)d;
        Trace.hit(0,context,token,first,second,third,null);
        build(context,token,first,second,third);
        Trace.hit(4,context,token,first,second,third,null);
    }
}
