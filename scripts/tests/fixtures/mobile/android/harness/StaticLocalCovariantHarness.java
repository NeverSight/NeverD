import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class StaticLocalCovariantHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned static local covariance"); }
    static Item read(int route,Api api,int index)throws Throwable {
        if(route==0)return Client.call(api,index);
        if(route==1)return StaticLocalCovariantCaller.call(api,index);
        try {
            if(route==2)return(Item)Api.class.getMethod("get",int.class).invoke(api,index);
            if(route==3||route==4) {
                Method selected=null;
                for(Method m:api.getClass().getDeclaredMethods())if(m.getName().equals("get")&&m.getReturnType()==(route==3?Item.class:Narrow.class))selected=m;
                require(selected!=null&&selected.isBridge()==(route==3)&&selected.isSynthetic()==(route==3));selected.setAccessible(true);return(Item)selected.invoke(api,index);
            }
        }catch(InvocationTargetException f){throw f.getCause();}
        MethodHandle m=MethodHandles.publicLookup().findVirtual(Api.class,"get",MethodType.methodType(Item.class,int.class));return(Item)m.invokeExact(api,index);
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int[] bits={0x7fc12345,0x80000000,0,0x7f800000,0xff800000,0x7f7fffff,1};
        for(int attempt=0;attempt<2;attempt++) {
            Object token=new Object();Narrow seed=mode==1?null:new Narrow(token);
            float first=Float.intBitsToFloat(bits[(mode+attempt)%bits.length]),second=Float.intBitsToFloat(bits[(mode+attempt+2)%bits.length]);
            Trace.events=0;Trace.seed=null;Trace.first=Trace.second=0;Trace.failAt=mode==5?1:mode==6?2:0;Trace.failure=new RuntimeException("owned local failure");
            Api api=null;Throwable caught=null;Item result=null;
            try {api=Owner.create(seed,first,second);if(mode==7)api=null;result=read(route,api,mode==2?1:mode==3?-1:mode==4?2:0);}catch(Throwable f){caught=f;}
            require(Trace.seed==seed&&Float.floatToRawIntBits(Trace.first)==Float.floatToRawIntBits(first)&&Float.floatToRawIntBits(Trace.second)==Float.floatToRawIntBits(second));
            long events=mode==5||mode==7?1:12;require(Trace.events==events);
            if(mode==5||mode==6)require(caught==Trace.failure);
            else if(mode==7)require(caught instanceof NullPointerException);
            else if(mode==3||mode==4)require(caught instanceof ArrayIndexOutOfBoundsException);
            else require(caught==null&&result==(mode==2?null:seed));
            if(result!=null)require(result.token()==token);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
