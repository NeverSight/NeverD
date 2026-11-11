import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class AllocationLongBitwiseHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned long bitwise allocation"); }
    static Target call(int route) throws Throwable {
        try {
            if(route==0)return Core.make();
            if(route==1)return AllocationLongBitwiseCaller.call();
            if(route==2)return (Target)Core.class.getMethod("make").invoke(null);
            return (Target)MethodHandles.publicLookup().findStatic(Core.class,"make",
                MethodType.methodType(Target.class)).invokeExact();
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    static long trace(int failure) {
        long events=0;
        for(int event:new int[]{1,2,1,2,1,2,1,3,1,3,1,3,4,5,6,7,8}) {
            events=events*10+event;if(event==failure)break;
        }
        return events;
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int[] shifts={0,1,31,32,63,64,65,-1,Integer.MIN_VALUE,Integer.MAX_VALUE};
        int[] failures={0,0,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8};
        RuntimeException original=new IllegalStateException("owned wide allocation failure");
        Target previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            long left=attempt==0?0x8123456789abcdefL:0x7fedcba987654321L;
            long right=attempt==0?0xfedcba9876543210L:0x0123456789abcdefL;
            int shift=shifts[mode%10];
            Trace.events=0;Trace.partial=null;Trace.left=left;Trace.right=right;Trace.shift=shift;
            Trace.failAt=failures[mode];Trace.failure=original;
            Target actual=null;
            try {actual=call(route);require(Trace.failAt==0);}
            catch(Throwable failure) {require(Trace.failAt!=0 && failure==original);}
            require(Trace.events==trace(Trace.failAt));
            boolean published=Trace.failAt==0 || Trace.failAt>=4;
            require((Trace.partial!=null)==published);
            if(published) {
                Pair partial=(Pair)Trace.partial;
                require(partial.a==(Trace.failAt==4?0:left&right));
                require(partial.b==(Trace.failAt==4?0:left|right));
                require(partial.c==(Trace.failAt==4?0:left^right));
                require(partial.d==(Trace.failAt==4||Trace.failAt==5?0:left<<shift));
                require(partial.e==(Trace.failAt==4||Trace.failAt==5?0:left>>shift));
                require(partial.f==(Trace.failAt==4||Trace.failAt==5?0:left>>>shift));
                if(Trace.failAt==0)require(actual!=previous && actual.pair==partial);
            }
            previous=actual;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
