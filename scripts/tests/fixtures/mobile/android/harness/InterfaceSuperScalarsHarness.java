import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InterfaceSuperScalarsHarness {
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned interface super scalar behavior"); }
    static int call(int route,Root receiver,float value,int view,String name) throws Throwable {
        try {
            if(route==0)return Facade.call(receiver,value,view);
            if(route==1)return InterfaceSuperScalarsCaller.call(receiver,value,view);
            if(route==2)return (Integer)Root.class.getMethod(name,float.class).invoke(receiver,value);
            MethodHandle handle=MethodHandles.publicLookup().findVirtual(Root.class,name,MethodType.methodType(int.class,float.class));
            return (int)handle.invokeExact(receiver,value);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);String name=args[2];boolean bridge=Boolean.parseBoolean(args[3]);
        for(Class<?> owner:new Class<?>[]{Owner.class,Direct.class}) {
            Method method=owner.getDeclaredMethod(name,float.class);
            check(method.isBridge()==bridge&&!method.isSynthetic()&&method.getReturnType()==int.class);
        }
        check(Api.class.getInterfaces().length==2&&Owner.class.getInterfaces().length==2);
        int[] inputs={0,0x80000000,0x7fc12345,0x7f800000,0xff800000,1,0xbf7fffff,0x7f7fffff};
        int bits=inputs[mode/4],requested=new int[]{0,2,3,4}[mode%4];float value=Float.intBitsToFloat(bits);
        for(int attempt=0;attempt<2;attempt++) {
            boolean child=attempt==0;Root receiver=Facade.make(child);Object sentinel=new Object();
            Trace.events=0;Trace.calls=0;Trace.seen=sentinel;Trace.input=Float.intBitsToFloat(0x12345678);
            Trace.failAt=requested;Trace.failure=new RuntimeException("owned scalar failure");
            Throwable caught=null;int result=0;
            try { result=call(route,receiver,value,mode%4,name); }catch(Throwable failure) { caught=failure; }
            boolean early=child&&requested==3,atDefault=!early,atFinish=atDefault&&requested!=2;
            long events=child?3:0;if(atDefault)events=events*10+2;if(atFinish)events=events*10+4;
            check(Trace.events==events&&Trace.calls==(atFinish?1:0));
            check(Trace.seen==(atFinish?receiver:sentinel));
            check(Float.floatToRawIntBits(Trace.input)==(atFinish?bits:0x12345678));
            boolean failure=early||requested==2||requested==4;
            check(failure?caught==Trace.failure:caught==null&&result==(bits^53));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
