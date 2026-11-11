package fixture;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class TypedConstructorIntegerDefaultHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned typed constructor integer default mismatch");
    }
    static Base<?> make(int route,boolean full,int left,Class token,int padding,int right) throws Throwable {
        if (route==1) return full ? TypedConstructorIntegerDefaultCaller.makeFull(left,token,padding,right)
            : TypedConstructorIntegerDefaultCaller.make(left,token,right);
        Base<?> result;
        if (route==0) result=full ? new Base<String>(left,token,padding,right) : new Base<String>(left,token,right);
        else if (route==2) try {
            result=(Base<?>)(full ? Base.class.getDeclaredConstructor(int.class,Class.class,int.class,int.class).newInstance(left,token,padding,right)
                : Base.class.getDeclaredConstructor(int.class,Class.class,int.class).newInstance(left,token,right));
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        else if (full) result=(Base<?>)MethodHandles.lookup().findConstructor(Base.class,
            MethodType.methodType(void.class,int.class,Class.class,int.class,int.class)).invokeExact(left,token,padding,right);
        else result=(Base<?>)MethodHandles.lookup().findConstructor(Base.class,
            MethodType.methodType(void.class,int.class,Class.class,int.class)).invokeExact(left,token,right);
        Trace.hit(4);
        return result;
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        boolean full=mode>=9;
        int left=mode==7 ? Integer.MIN_VALUE : 23,right=mode==8 ? -2147483647 : -29,padding=full ? 37 : 0;
        Class token=mode==5||mode==10 ? null : mode==6 ? Integer.class : String.class;
        Trace.fail=mode>=1&&mode<=4 ? mode : mode==11 ? 2 : 0;
        Trace.failure=new IllegalArgumentException("owned typed constructor failure");
        for (int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.parent=null;
            Throwable failure=null;Base<?> result=null;
            try { result=make(route,full,left,token,padding,right); } catch (Throwable actual) { failure=actual; }
            int end=attempt==1&&mode==1 ? 0 : Trace.fail>0 ? Trace.fail : 4;
            long expected=0;
            for (int event=attempt==0 ? 1 : 2;event<=end;event++) {
                if (full&&event==3) continue;
                expected=expected*17+event;
            }
            require(Trace.events==expected);
            require((Trace.parent!=null)==(end>=2));
            if (end>=2) {
                Base<?> partial=(Base<?>)Trace.parent;
                require(partial.left==left && partial.right==right && partial.padding==padding && partial.token==token);
            }
            if (mode==1) {
                if (attempt==0) require(failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (Trace.fail>0) require(failure==Trace.failure);
            else require(failure==null && result==Trace.parent);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
