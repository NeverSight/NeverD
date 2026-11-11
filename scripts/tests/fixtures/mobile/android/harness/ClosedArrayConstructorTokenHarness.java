import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedArrayConstructorTokenHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed array constructor mismatch");
    }
    static Base<?> make(int route,int left,Class token,int right) throws Throwable {
        if (route==0) return Core.make(left,token,right);
        if (route==1) return ClosedArrayConstructorTokenCaller.make(left,token,right);
        if (route==2) try { return (Base<?>)Core.class.getMethod("make",int.class,Class.class,int.class).invoke(null,left,token,right); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
        return (Base<?>)MethodHandles.publicLookup().findStatic(Core.class,"make",
            MethodType.methodType(Base.class,int.class,Class.class,int.class)).invokeExact(left,token,right);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int left=mode==7 ? Integer.MIN_VALUE : 23,right=mode==8 ? -2147483647 : -29;
        Class token=mode==5 ? null : mode==6 ? Integer.class : float[].class;
        Trace.fail=mode>=9 ? new int[]{2,3,1,4}[mode-9] : mode>=1&&mode<=4 ? mode : 0;
        Trace.error=mode>=9 ? new LinkageError("owned array constructor error") : null;
        Trace.failure=new IllegalArgumentException("owned local constructor failure");
        for (int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.parent=null;
            Throwable failure=null;Base<?> result=null;
            try { result=make(route,left,token,right); } catch (Throwable actual) { failure=actual; }
            int end=attempt==1&&(mode==1||mode==11) ? 0 : Trace.fail!=0 ? Trace.fail : 4;
            long expected=0;
            for (int event=attempt==0 ? 1 : 2;event<=end;event++) expected=expected*17+event;
            require(Trace.events==expected);
            require((Trace.parent!=null)==(end>=2));
            if (end>=2) {
                Base<?> partial=(Base<?>)Trace.parent;
                require(partial.left==left && partial.right==right && partial.padding==0 && partial.token==token);
            }
            if (mode==1||mode==11) {
                if (attempt==0) require(mode==11 ? failure==Trace.error : failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode>=2&&mode<=4||mode>=9) require(failure==(Trace.error!=null ? Trace.error : Trace.failure));
            else require(failure==null && result==Trace.parent);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
