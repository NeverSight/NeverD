import fixture.*;
public class EncodedStaticPrefixHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned encoded static prefix");}
    static void values(Core value,int a,long b,float c,double d,Object token) {
        require(value!=null&&value.a==a&&value.b==b&&Float.floatToRawIntBits(value.c)==Float.floatToRawIntBits(c)&&Double.doubleToRawLongBits(value.d)==Double.doubleToRawLongBits(d)&&value.token==token);
    }
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int raw=mode==21?0x80000000:mode==22?0x7fc01234:0x3f800001;
        long wide=mode==21?0x8000000000000000L:mode==22?0x7ff8123456789abcL:0x3ff0000000000001L;
        Trace.seedA=0x13579bdf;Trace.seedB=Long.MIN_VALUE+17;Trace.seedC=Float.intBitsToFloat(raw);Trace.seedD=Double.longBitsToDouble(wide);
        Trace.reenter=mode==19;Trace.fail=mode==16?5:mode==17?6:mode==18?7:0;
        Trace.failure=mode==18?new AssertionError("owned error"):new IllegalStateException("owned runtime");
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.partial=null;
            Object token=mode==20?null:new Object();
            int a=Integer.MIN_VALUE+attempt,mask=route==3?15:mode<16?mode:15;
            long b=0x12345678fedcba98L+attempt;float c=Float.intBitsToFloat(0xffc04321);double d=Double.longBitsToDouble(0xfff8432100000001L);
            Core value=null;Throwable failure=null;
            try{value=EncodedStaticPrefixCaller.make(route,a,b,c,d,token,mask);}catch(Throwable caught){failure=caught;}
            long events=attempt==0?(mode==19?1256734:1234):0;
            events=events*10+5;if(Trace.fail!=5){events=events*10+6;if(Trace.fail!=6)events=events*10+7;}
            require(Trace.events==events&&failure==(Trace.fail==0?null:Trace.failure));
            if(Trace.fail==5)require(Trace.partial==null&&value==null);
            else {
                require(Trace.partial!=null&&(Trace.fail!=0||Trace.partial==value));
                if(Trace.fail==6)values(Trace.partial,0,0L,0.0f,0.0d,null);
                else values(Trace.partial,(mask&1)!=0?Trace.seedA:a,(mask&2)!=0?Trace.seedB:b,(mask&4)!=0?Trace.seedC:c,(mask&8)!=0?Trace.seedD:d,route==3?null:token);
            }
            if(mode==19)values(Trace.nested,Trace.seedA,0L,0.0f,0.0d,null);
            else require(Trace.nested==null);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+events);
        }
    }
}
