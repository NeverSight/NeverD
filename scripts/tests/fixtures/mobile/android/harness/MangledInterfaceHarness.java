import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
public final class MangledInterfaceHarness {
    static void require(boolean value) { if(!value) throw new AssertionError("mangled virtual dispatch changed"); }
    static Object call(Relay<Object> receiver, Object value, int route, String name) throws Throwable {
        if(route == 0) return NamesKt.call(receiver, value);
        if(route == 1) return MangledInterfaceCaller.inherited(receiver, value);
        if(route == 2) return MangledInterfaceCaller.concrete(receiver, value);
        if(route == 3) {
            try { return Api.class.getMethod(name, Object.class).invoke(receiver, value); }
            catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findVirtual(Api.class, name,
                MethodType.methodType(Object.class, Object.class)).invoke(receiver, value);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        String name = args[3];
        require(!Api.class.getMethod(name,Object.class).isSynthetic());
        require(Api.class.getTypeParameters().length == 1 && Child.class.getTypeParameters().length == 1);
        Trace.failure = new RuntimeException("owned virtual failure");
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : 0;
        int[] input = {7}, calls = {0};
        Object value = mode == 4 ? null : input;
        Object output = mode == 5 ? null : input;
        Api<Object> target = (Api<Object>)Proxy.newProxyInstance(Api.class.getClassLoader(), new Class<?>[]{Api.class},
                (proxy,method,values) -> {
                    require(method.getName().equals(name) && values.length == 1 && values[0] == value);
                    calls[0]++; Trace.hit(3); input[0]++; return output;
                });
        Relay<Object> receiver = variant == 0 ? new Relay<Object>(target) : new Child<Object>(target);
        if(mode == 6) receiver = null;
        for(int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; int oldCalls=calls[0], oldValue=input[0];
            Throwable failure=null; Object result=null;
            try { result=call(receiver,value,route,name); } catch(Throwable error) { failure=error; }
            int expected = mode == 6 ? 0 : variant == 1 && mode == 1 ? 1 :
                    mode == 2 ? (variant == 1 ? 12 : 2) : (variant == 1 ? 123 : 23);
            boolean callback = mode != 6 && mode != 2 && !(variant == 1 && mode == 1);
            boolean throwing = mode == 3 || mode == 2 || variant == 1 && mode == 1;
            require(Trace.events == expected && calls[0] == oldCalls + (callback ? 1 : 0));
            require(input[0] == oldValue + (callback && mode != 3 ? 1 : 0));
            if(mode == 6) require(failure instanceof NullPointerException);
            else if(throwing) require(failure == Trace.failure);
            else require(failure == null && result == output);
            System.out.println("variant="+variant+",route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
