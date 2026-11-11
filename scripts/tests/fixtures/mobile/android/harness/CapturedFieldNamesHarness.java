import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.function.LongSupplier;
public final class CapturedFieldNamesHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned captured field"); }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        long[] values={0L,-1L,Long.MIN_VALUE,Long.MAX_VALUE,0x0123456789abcdefL,0x7ff0000000000001L};
        long wide=values[mode%values.length];Object token=mode==8?null:new Object();
        Object sentinel=new Object();Trace.failure=new RuntimeException("owned capture failure");
        Hook hook=mode==7?null:(bits,incoming)->{
            Trace.calls++;Trace.hit(4);require(bits==wide&&incoming==token);
            Trace.next+=0x0102030405060708L;return mode==9?bits:bits^Trace.next;
        };
        Core core=new Core();LongSupplier supplier=CapturedFieldNamesCaller.make(core,wide,token,hook);
        LongSupplier independent=core.make(~wide,sentinel,(bits,incoming)->{require(bits==~wide&&incoming==sentinel);return bits;});
        require(supplier!=independent);
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.token=Trace.hook=null;Trace.wide=~wide;
            Trace.failAt=mode>=1&&mode<=4?mode:0;
            long before=Trace.next;int calls=Trace.calls;Throwable failure=null;long result=0;
            try {
                if(route==0)result=supplier.getAsLong();
                else if(route==1)result=CapturedFieldNamesCaller.call(supplier);
                else if(route==2)result=(Long)LongSupplier.class.getMethod("getAsLong").invoke(supplier);
                else result=(long)MethodHandles.publicLookup().findVirtual(LongSupplier.class,"getAsLong",MethodType.methodType(long.class)).invoke(supplier);
            } catch(InvocationTargetException error) { failure=error.getCause(); }
              catch(Throwable error) { failure=error; }
            String steps=mode==1?"1":mode==2||mode==7?"12":mode==4?"124":"1243";
            long expected=Long.parseLong(steps);require(Trace.events==expected);
            require(Trace.calls==calls+(steps.indexOf('4')>=0?1:0));
            require(Trace.token==(mode==1?null:token)&&Trace.hook==(mode==1?null:hook)&&Trace.wide==(mode==1?~wide:wide));
            if(mode>=1&&mode<=4)require(failure==Trace.failure);
            else if(mode==7)require(failure instanceof NullPointerException);
            else require(failure==null&&result==(mode==9?wide:wide^(before+0x0102030405060708L)));
            if(attempt==0) {
                Trace.failAt=0;require(independent.getAsLong()==~wide);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
