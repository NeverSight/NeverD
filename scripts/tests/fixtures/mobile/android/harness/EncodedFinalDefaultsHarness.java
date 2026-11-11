import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class EncodedFinalDefaultsHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("encoded final defaults contract");
    }
    static int read(int route) throws Throwable {
        if (route == 0) return Client.read();
        if (route == 1) return EncodedFinalCaller.read();
        if (route == 2) return Owner.class.getField("aI").getInt(null);
        if (route == 3) return (int)MethodHandles.lookup().findStaticGetter(Alias.class,"aI",int.class).invokeExact();
        return FreshEncodedFinalCaller.read();
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Object seed = new Object(); Object[] array = {seed};
        Trace.z = true; Trace.b = (byte)0x81; Trace.c = (char)0xdfff; Trace.s = (short)0x8765;
        Trace.i = 0x87654321; Trace.j = 0xfedcba9876543210L;
        Trace.f = Float.intBitsToFloat(mode % 2 == 0 ? 0x7fc01234 : 0x80000000);
        Trace.d = Double.longBitsToDouble(mode % 2 == 0 ? 0x7ff8123456789abcL : 0x8000000000000000L);
        Trace.seed = seed; Trace.array = array;
        Trace.failure = new RuntimeException("owned encoded default failure"); Trace.failAt = mode;
        for (int attempt=0; attempt<2; attempt++) {
            Trace.events = 0;
            int result=0; Throwable failure=null;
            try { result=read(route); } catch (Throwable error) { failure=error; }
            int events = mode==1 ? 1 : mode==2 ? 142 : mode==4 ? 14 : mode==5 ? 1425 : 14253;
            require(Trace.events == (attempt==0 ? events : 0));
            require(Trace.bad==0 && Trace.observations==(mode==1 ? 0 : mode==2 || mode==4 ? 1 : 2));
            require(Trace.first==(mode!=1 && mode!=4));
            require(Trace.second==(mode==0 || mode==3));
            if (mode==0) {
                require(failure==null && result==0x87654321 && Owner.aRef==seed && Owner.aArray==array);
                require(Owner.aZ && Owner.aB==(byte)0x81 && Owner.aC==(char)0xdfff && Owner.aS==(short)0x8765);
                require(Owner.aJ==0xfedcba9876543210L && Float.floatToRawIntBits(Owner.aF)==0x7fc01234
                    && Double.doubleToRawLongBits(Owner.aD)==0x7ff8123456789abcL);
                array[0]=new Object(); require(Owner.aArray[0]==array[0]);
            } else if (attempt==0) require(failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure);
            else require(failure instanceof NoClassDefFoundError);
            for (Field field : Owner.class.getFields())
                if (field.getName().startsWith("a")) require(Modifier.isFinal(field.getModifiers()) && Modifier.isStatic(field.getModifiers()));
            Trace.seed=new Object(); Trace.array=new Object[1]; Trace.i=0;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
