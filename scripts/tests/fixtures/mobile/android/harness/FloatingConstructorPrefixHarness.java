import fixture.*;
import java.lang.invoke.*;
public class FloatingConstructorPrefixHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned floating constructor prefix");}
    static Core make(int route,boolean chain,boolean simple,float a,float b,float e,double c,double d,Object context) throws Throwable {
        if(simple) {
            if(route==0)return new Core(a,b,e,0);
            if(route==1)return FloatingConstructorPrefixCaller.make(chain,true,a,b,e,c,d,context);
            MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,float.class,float.class,float.class,int.class));
            if(route==2)return (Core)h.invokeWithArguments(a,b,e,0);
            return (Core)h.invokeExact(a,b,e,0);
        }
        if(route==0)return chain?new Core(a,b,e,c,d,context):new Core(a,b,c,d,context);
        if(route==1)return FloatingConstructorPrefixCaller.make(chain,false,a,b,e,c,d,context);
        MethodType type=chain?MethodType.methodType(void.class,float.class,float.class,float.class,double.class,double.class,Object.class):MethodType.methodType(void.class,float.class,float.class,double.class,double.class,Object.class);
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,type);
        if(route==2)return chain?(Core)h.invokeWithArguments(a,b,e,c,d,context):(Core)h.invokeWithArguments(a,b,c,d,context);
        return chain?(Core)h.invokeExact(a,b,e,c,d,context):(Core)h.invokeExact(a,b,c,d,context);
    }
    static String f(float value){return Float.isNaN(value)?"nan":Integer.toHexString(Float.floatToRawIntBits(value));}
    static String d(double value){return Double.isNaN(value)?"nan":Long.toHexString(Double.doubleToRawLongBits(value));}
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int[] fa={0x3f800000,0x80000000,0,0x7fc01234,0x7f800000,0xff800000,0x7f7fffff,0x4b800000};
        int[] fb={0x40000000,0,0x80000000,0x3f800000,0x40000000,0x7f800000,0x7f7fffff,0x3f800000};
        int[] fe={0x40400000,0x80000000,0,0x40000000,0,0xff800000,0xff7fffff,0xcb800000};
        long[] dc={0x3ff3c0ca428c59fbL,0x8000000000000000L,0,0x7ff8000000001234L,0x7ff0000000000000L,0xfff0000000000000L,0x7fefffffffffffffL,0x4340000000000000L};
        long[] dd={0x4004000000000000L,0,0x8000000000000000L,0x3ff0000000000000L,0x4000000000000000L,0x7ff0000000000000L,0x7fefffffffffffffL,0x3ff0000000000000L};
        int pattern=mode<8?mode:mode<12?7:mode==12?7:mode==13?6:mode==14?0:1;
        boolean chain=mode>=12;
        int fail=mode>=8&&mode<12?mode-7:0;
        Core previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            float a=Float.intBitsToFloat(fa[pattern]),b=Float.intBitsToFloat(fb[pattern]),e=Float.intBitsToFloat(fe[pattern]);
            double c=Double.longBitsToDouble(dc[pattern]),d=Double.longBitsToDouble(dd[pattern]);
            String name=mode==6||mode==14?null:new String("owned original");
            Object context=mode==7||mode==15?null:mode==5?new Object[]{new Object()}:new Object();
            Trace.partial=null;Trace.events=0;Trace.names=0;Trace.fail=fail;Trace.label=name;Trace.changedLabel=new String("owned changed");
            Trace.failure=attempt==0?new IllegalStateException("owned runtime"):new AssertionError("owned error");
            Core value=null;Throwable failure=null;
            try{value=make(route,chain,mode==15,a,b,e,c,d,context);}catch(Throwable error){failure=error;}
            long events=fail==0||fail==4?1234:fail==1?1:fail==2?12:123;
            require(Trace.events==events&&Trace.names==1&&Trace.label==Trace.changedLabel);
            require(failure==(fail==0?null:Trace.failure));
            require(fail==1?Trace.partial==null:value==null||value==Trace.partial);
            String values="none";
            if(Trace.partial!=null) {
                Core p=Trace.partial;require(p!=previous);previous=p;
                boolean complete=fail==0||fail==4;
                require(p.name==(complete?name:null)&&p.context==(complete?context:null));
                require(fail==0?value==p:value==null);
                values=f(p.add)+":"+f(p.sub)+":"+f(p.mul)+":"+f(p.div)+":"+f(p.rem)+":"+d(p.wideAdd)+":"+d(p.wideSub)+":"+d(p.wideMul)+":"+d(p.wideDiv)+":"+d(p.wideRem);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events+",values="+values);
        }
    }
}
