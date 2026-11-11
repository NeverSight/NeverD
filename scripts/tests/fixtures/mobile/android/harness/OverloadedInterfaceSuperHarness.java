import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class OverloadedInterfaceSuperHarness {
    static void check(boolean ok) { InterfaceSuperScalarsHarness.check(ok); }
    static float callInt(int route,Root receiver,int value,int view,String name) throws Throwable {
        try {
            if(route==0)return Facade.callInt(receiver,value,view);
            if(route==1)return OverloadedInterfaceSuperCaller.callInt(receiver,value,view);
            if(route==2)return (Float)Root.class.getMethod(name,int.class).invoke(receiver,value);
            return (float)MethodHandles.publicLookup().findVirtual(Root.class,name,
                    MethodType.methodType(float.class,int.class)).invokeExact(receiver,value);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    static double callLong(int route,Root receiver,long value,int view,String name) throws Throwable {
        try {
            if(route==0)return Facade.callLong(receiver,value,view);
            if(route==1)return OverloadedInterfaceSuperCaller.callLong(receiver,value,view);
            if(route==2)return (Double)Root.class.getMethod(name,long.class).invoke(receiver,value);
            return (double)MethodHandles.publicLookup().findVirtual(Root.class,name,
                    MethodType.methodType(double.class,long.class)).invokeExact(receiver,value);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        InterfaceSuperScalarsHarness.main(args);
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        String name=args[2];boolean bridge=Boolean.parseBoolean(args[3]);
        for(Class<?> owner:new Class<?>[]{Owner.class,Direct.class})
            for(Class<?> type:new Class<?>[]{int.class,long.class}) {
                Method method=owner.getDeclaredMethod(name,type);
                check(method.isBridge()==bridge&&!method.isSynthetic());
                check(method.getReturnType()==(type==int.class?float.class:double.class));
            }
        int[] ints={0x01000001,0x80000000,0x7fc12345,0x7f800000,0xff800000,1,0xbf7fffff,0x7f7fffff};
        long[] longs={0x1234567812345678L,Long.MIN_VALUE,0x7ff8123456789abcL,0x7ff0000000000000L,0xfff0000000000000L,1,-1,Long.MAX_VALUE};
        for(int kind=0;kind<2;kind++)for(int attempt=0;attempt<2;attempt++) {
            int event=kind==0?1:2;
            int requested=new int[]{0,event,3,4}[mode%4],iv=ints[mode/4];long lv=longs[mode/4];
            boolean child=attempt==0;Root receiver=Facade.make(child);Object sentinel=new Object();
            Trace.events=0;Trace.calls=0;Trace.seen=sentinel;Trace.inputInt=91;Trace.inputLong=93;
            Trace.failAt=requested;Trace.failure=new RuntimeException("owned overloaded scalar failure");
            Throwable caught=null;long result=0;
            try {
                if(kind==0)result=Float.floatToRawIntBits(callInt(route,receiver,iv,mode%4,name));
                else result=Double.doubleToRawLongBits(callLong(route,receiver,lv,mode%4,name));
            } catch(Throwable failure) { caught=failure; }
            boolean early=child&&requested==3,atDefault=!early,atFinish=atDefault&&requested!=event;
            long events=child?3:0;if(atDefault)events=events*10+event;if(atFinish)events=events*10+4;
            check(Trace.events==events&&Trace.calls==(atFinish?1:0));
            check(Trace.seen==(atFinish?receiver:sentinel));
            check(Trace.inputInt==(kind==0&&atFinish?iv:91));
            check(Trace.inputLong==(kind==1&&atFinish?lv:93));
            boolean failure=early||requested==event||requested==4;
            check(failure?caught==Trace.failure:caught==null&&result==(kind==0?iv:lv));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
