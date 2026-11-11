import fixture.*;
import java.lang.invoke.*;
public class AllocationPrefixCopyHarness {
    static void require(boolean value) { if(!value)throw new AssertionError("owned allocation prefix copies"); }
    static final Class<?>[] TYPES={boolean.class,byte.class,char.class,short.class,int.class,long.class,float.class,double.class,Object.class,Object[].class};
    static Core make(int route,int type,Object input,Object secondInput,double carry) throws Throwable {
        if(route==1)return AllocationPrefixCopyCaller.make(TYPES[type],input,secondInput,carry);
        if(route==2) {
            MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,double.class,double.class,double.class,double.class,double.class,double.class,TYPES[type],TYPES[type]));
            return (Core)h.invokeWithArguments(carry,2.0,3.0,4.0,5.0,6.0,input,secondInput);
        }
        if(route==3) {
            MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,double.class,double.class,double.class,double.class,double.class,double.class,TYPES[type],TYPES[type]));
            if(type==0)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Boolean)input).booleanValue(),((Boolean)secondInput).booleanValue());
            if(type==1)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Byte)input).byteValue(),((Byte)secondInput).byteValue());
            if(type==2)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Character)input).charValue(),((Character)secondInput).charValue());
            if(type==3)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Short)input).shortValue(),((Short)secondInput).shortValue());
            if(type==4)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Integer)input).intValue(),((Integer)secondInput).intValue());
            if(type==5)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Long)input).longValue(),((Long)secondInput).longValue());
            if(type==6)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Float)input).floatValue(),((Float)secondInput).floatValue());
            if(type==7)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,((Double)input).doubleValue(),((Double)secondInput).doubleValue());
            if(type==8)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,input,secondInput);
            if(type==9)return (Core)h.invokeExact(carry,2.0,3.0,4.0,5.0,6.0,(Object[])input,(Object[])secondInput);
        }
        if(type==0)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Boolean)input).booleanValue(),((Boolean)secondInput).booleanValue());
        if(type==1)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Byte)input).byteValue(),((Byte)secondInput).byteValue());
        if(type==2)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Character)input).charValue(),((Character)secondInput).charValue());
        if(type==3)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Short)input).shortValue(),((Short)secondInput).shortValue());
        if(type==4)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Integer)input).intValue(),((Integer)secondInput).intValue());
        if(type==5)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Long)input).longValue(),((Long)secondInput).longValue());
        if(type==6)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Float)input).floatValue(),((Float)secondInput).floatValue());
        if(type==7)return new Core(carry,2.0,3.0,4.0,5.0,6.0,((Double)input).doubleValue(),((Double)secondInput).doubleValue());
        if(type==8)return new Core(carry,2.0,3.0,4.0,5.0,6.0,input,secondInput);
        if(type==9)return new Core(carry,2.0,3.0,4.0,5.0,6.0,(Object[])input,(Object[])secondInput);
        throw new AssertionError();
    }
    static void check(Value value,int kind,long bits,Object reference,boolean stored) {
        require(value!=null && value.kind==(stored?kind:0) && value.bits==(stored?bits:0) && value.reference==(stored?reference:null));
    }
    static Object[] sample(int type,int pattern) {
        long[] longs={0,-1,Long.MIN_VALUE,Long.MAX_VALUE,0x12345678fedcba98L,1,0x8000000000000001L,0x7ff8000000001234L};
        int[] floats={0,0x80000000,0x7fc01234,0xffc04321,0x7f800000,0xff800000,1,0x3f800001};
        long[] doubles={0,0x8000000000000000L,0x7ff8000000001234L,0xfff8000000004321L,0x7ff0000000000000L,0xfff0000000000000L,1,0x3ff0000000000001L};
                Object input;long bits=0;Object reference=null;
                switch(type) {
                    case 0:input=Boolean.valueOf(pattern%2!=0);bits=pattern%2;break;
                    case 1:input=Byte.valueOf((byte)longs[pattern]);bits=(byte)longs[pattern];break;
                    case 2:input=Character.valueOf((char)longs[pattern]);bits=(char)longs[pattern];break;
                    case 3:input=Short.valueOf((short)longs[pattern]);bits=(short)longs[pattern];break;
                    case 4:input=Integer.valueOf((int)longs[pattern]);bits=(int)longs[pattern];break;
                    case 5:input=Long.valueOf(longs[pattern]);bits=longs[pattern];break;
                    case 6:input=Float.valueOf(Float.intBitsToFloat(floats[pattern]));bits=(long)floats[pattern];break;
                    case 7:input=Double.valueOf(Double.longBitsToDouble(doubles[pattern]));bits=doubles[pattern];break;
                    case 8:input=pattern==0?null:pattern==1?new Object[0]:new Object();reference=input;break;
                    default:input=pattern==0?null:pattern==1?new Object[0]:new Object[]{new Object(),null};reference=input;break;
                }
        return new Object[]{input,Long.valueOf(bits),reference};
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int type=mode<10?mode:(mode%2==0?8:7),fail=mode<10?0:mode-9;
        long[] longs={0,-1,Long.MIN_VALUE,Long.MAX_VALUE,0x12345678fedcba98L,1,0x8000000000000001L,0x7ff8000000001234L};
        int[] floats={0,0x80000000,0x7fc01234,0xffc04321,0x7f800000,0xff800000,1,0x3f800001};
        long[] doubles={0,0x8000000000000000L,0x7ff8000000001234L,0xfff8000000004321L,0x7ff0000000000000L,0xfff0000000000000L,1,0x3ff0000000000001L};
        Value previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            for(int pattern=0;pattern<8;pattern++) {
                Object[] firstSample=sample(type,pattern),secondSample=sample(type,(pattern+1)%8);
                Object input=firstSample[0],secondInput=secondSample[0],reference=firstSample[2],secondReference=secondSample[2];
                long bits=((Long)firstSample[1]).longValue(),secondBits=((Long)secondSample[1]).longValue();
                Trace.events=0;Trace.allocations=0;Trace.first=null;Trace.second=null;Trace.partial=null;Trace.other=new Object();
                Trace.fail=fail;Trace.failure=pattern%2==0?new IllegalStateException("owned runtime"):new AssertionError("owned error");
                double carry=Double.longBitsToDouble(doubles[pattern]);
                Core result=null;Throwable failure=null;
                try{result=make(route,type,input,secondInput,carry);}catch(Throwable error){failure=error;}
                long events=fail==0?123456:0;
                if(fail!=0)for(int i=1;i<=fail;i++)events=events*10+i;
                require(Trace.events==events && Trace.allocations==(fail==1||fail==2?1:2));
                check(Trace.first,type,bits,reference,fail!=1);
                require(Trace.first!=previous);previous=Trace.first;
                if(fail!=1&&fail!=2){require(Trace.second!=Trace.first);check(Trace.second,type,secondBits,secondReference,fail!=3);}else require(Trace.second==null);
                boolean primary=fail==0||fail>=5;
                require((Trace.partial!=null)==primary);
                if(primary)require(Trace.partial.first==(fail==5?null:Trace.first) && Trace.partial.second==(fail==5?null:Trace.second) && Double.doubleToRawLongBits(Trace.partial.carry)==(fail==5?0:doubles[pattern]));
                require(failure==(fail==0?null:Trace.failure) && (fail==0?result==Trace.partial:result==null));
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
