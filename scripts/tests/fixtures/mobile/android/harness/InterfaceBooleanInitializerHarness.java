package fixture;
import java.lang.invoke.*;
public final class InterfaceBooleanInitializerHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned interface boolean initializer mismatch");
    }
    static boolean read(int route) throws Throwable {
        if (route==1) return InterfaceBooleanInitializerCaller.read();
        boolean result;
        if (route==0) result=Policy.enabled;
        else if (route==2) result=Policy.class.getField("enabled").getBoolean(null);
        else result=(boolean)MethodHandles.lookup().findStaticGetter(Policy.class,"enabled",boolean.class).invokeExact();
        Trace.hit(4);
        return result;
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int left=mode==5 ? 26 : mode==6 ? 27 : mode==7 ? Integer.MIN_VALUE
            : mode==8 ? Integer.MAX_VALUE : mode==9 ? -3 : 31;
        int right=mode==7 ? 0 : mode==8 ? Integer.MIN_VALUE : mode==9 ? -4 : 27;
        boolean expectedValue=left>=right;
        Trace.fail=mode>=1&&mode<=4 ? mode : 0;
        Trace.failure=new IllegalArgumentException("owned interface initialization failure");
        for (int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.defaultSeen=true;
            int initial=attempt==0 ? left : expectedValue ? -1000 : 1000;
            Trace.left=initial;Trace.right=right;Trace.late=expectedValue ? -2000 : 2000;
            boolean result=false;Throwable failure=null;
            try { result=read(route); } catch (Throwable actual) { failure=actual; }
            boolean initializationFailure=mode>=1&&mode<=3;
            int end=attempt==1&&initializationFailure ? 0 : Trace.fail>0 ? Trace.fail : 4;
            long expected=0;
            for (int event=attempt==0 ? 1 : 4;event<=end;event++) expected=expected*17+event;
            require(Trace.events==expected);
            boolean leftRan=attempt==0&&end>=2,rightRan=attempt==0&&end>=3;
            require(Trace.defaultSeen==!leftRan);
            require(Trace.left==(rightRan ? Trace.late : initial));
            if (initializationFailure) {
                if (attempt==0) require(failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode==4) {
                require(failure==Trace.failure);
                require(Policy.class.getField("enabled").getBoolean(null)==expectedValue);
            } else require(failure==null && result==expectedValue);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
