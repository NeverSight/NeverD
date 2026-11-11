import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class CheckedReferenceReturnHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned checked reference return contract"); }
    static Object call(int route, Core receiver, Object input, Box callback, long wide, String name) throws Throwable {
        if (route == 0) return Client.call(receiver, input, callback, wide);
        if (route == 1) return CheckedReferenceReturnCaller.call(receiver, input, callback, wide);
        if (route == 2) {
            try { return Api.class.getMethod(name, Object.class, Box.class, long.class).invoke(receiver, input, callback, wide); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findVirtual(Api.class, name,
            MethodType.methodType(Result.class, Object.class, Box.class, long.class)).invoke(receiver, input, callback, wide);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        String name=args[2], targetName=args[3];
        Method target=null, bridge=null;
        for (Method method : Core.class.getDeclaredMethods()) {
            if (method.getName().equals(targetName) && method.getReturnType() == Void.class) target=method;
            if (method.getName().equals(name) && method.getReturnType() == Result.class) bridge=method;
        }
        require(target!=null && bridge!=null);
        require(((ParameterizedType)target.getGenericParameterTypes()[1]).getActualTypeArguments()[0] instanceof WildcardType);
        require(bridge.getGenericParameterTypes()[1].equals(Box.class));
        long wide=mode==6 ? Long.MIN_VALUE : mode==7 ? Long.MAX_VALUE : 0x123456789abcdef0L;
        int[] data={7}, calls={0};
        Object input=mode==5 ? null : data, result=mode==8 ? null : new Object();
        Core receiver=mode==4 ? null : new Core();
        Trace.failure=new IllegalArgumentException("owned reference failure");
        Trace.fail=mode>=1 && mode<=3 ? mode : 0;
        Box<Object> callback=mode==9 ? null : (value,number) -> {
            require(value==input && number==wide); calls[0]++; Trace.hit(2);
            if (value!=null) ((int[])value)[0]++; return result;
        };
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0; Object prior=new Object(); Trace.result=prior;
            int oldCalls=calls[0], oldData=data[0];
            Throwable failure=null; Object actual=new Object();
            try { actual=call(route,receiver,input,callback,wide,name); } catch(Throwable caught){failure=caught;}
            boolean entered=mode!=1 && mode!=4 && mode!=9;
            boolean stored=entered && mode!=2;
            long expected=mode==4 ? 0 : stored ? 123 : entered ? 12 : 1;
            if(mode>=1 && mode<=3) require(failure==Trace.failure);
            else if(mode==4 || mode==9) require(failure instanceof NullPointerException);
            else require(failure==null && actual==null);
            require(Trace.events==expected && calls[0]==oldCalls+(entered?1:0));
            require(Trace.result==(stored?result:prior));
            require(data[0]==oldData+(stored && input!=null?1:0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
