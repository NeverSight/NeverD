import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class PrivateInstanceNamesHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned private instance name contract"); }
    static long call(int route, Provider<?> receiver, long first, long second) throws Throwable {
        if(route==0) return receiver.call(first,second);
        if(route==1) return PrivateInstanceNamesCaller.call(receiver,first,second);
        if(route==2) {
            try { return (Long)Provider.class.getMethod("call",long.class,long.class).invoke(receiver,first,second); }
            catch(InvocationTargetException failure){throw failure.getCause();}
        }
        return (long)MethodHandles.publicLookup().findVirtual(Provider.class,"call",MethodType.methodType(long.class,long.class,long.class)).invoke(receiver,first,second);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), kind=Integer.parseInt(args[1]), mode=Integer.parseInt(args[2]);
        Method privateMethod=Provider.class.getDeclaredMethod(args[3],long.class,long.class);
        Method publicMethod=Child.class.getDeclaredMethod(args[4],long.class,long.class);
        require(Modifier.isPrivate(privateMethod.getModifiers()) && Modifier.isFinal(privateMethod.getModifiers()));
        require(Modifier.isPublic(publicMethod.getModifiers()) && Modifier.isFinal(publicMethod.getModifiers()));
        Field seed=Provider.class.getDeclaredField("seed"); seed.setAccessible(true);
        Provider<?> receiver=mode==4 ? null : kind==0 ? new Provider<Object>(17) : new Child(17);
        long first=mode==5 ? Long.MIN_VALUE : mode==7 ? Long.MAX_VALUE : 0x123456789abcdef0L;
        long second=mode==6 || mode==7 ? Long.MAX_VALUE : -0x4321L;
        Trace.failure=new IllegalArgumentException("owned private instance failure");
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0; Trace.fail=mode>=1 && mode<=3 ? mode : 0;
            Trace.result=41; long before=receiver==null ? 0 : seed.getLong(receiver);
            long expectedSeed=mode==1 || mode==4 ? before : before+first;
            boolean stored=mode!=1 && mode!=2 && mode!=4;
            long wanted=expectedSeed+second, actual=99; Throwable failure=null;
            try { actual=call(route,receiver,first,second); } catch(Throwable caught){failure=caught;}
            if(mode>=1 && mode<=3) require(failure==Trace.failure);
            else if(mode==4) require(failure instanceof NullPointerException);
            else require(failure==null && actual==wanted);
            int expected=mode==4 ? 0 : mode==1 ? 1 : mode==2 ? 12 : 123;
            require(Trace.events==expected && Trace.result==(stored?wanted:41));
            if(receiver!=null) require(seed.getLong(receiver)==expectedSeed);
            if(receiver instanceof Child) {
                Trace.events=0; Trace.fail=mode==8 ? 4 : 0;
                Throwable childFailure=null; long other=99;
                try { other=((Child)receiver).other(first,second); } catch(Throwable caught){childFailure=caught;}
                require(Trace.events==4 && seed.getLong(receiver)==expectedSeed);
                if(mode==8) require(childFailure==Trace.failure);
                else require(childFailure==null && other==first-second);
            }
            System.out.println("route="+route+",kind="+kind+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
