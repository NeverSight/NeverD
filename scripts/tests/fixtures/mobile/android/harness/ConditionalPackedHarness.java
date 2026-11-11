import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConditionalPackedHarness {
    static void require(boolean value) { if(!value)throw new AssertionError("owned conditional packed arguments"); }
    static Target call(int route,Creator creator,Object left,Object right,float first,float second,int shift,long mask,int low,int high) throws Throwable {
        if(route==0)return creator.make(left,right,first,second,shift,mask,low,high);
        if(route==1)return ConditionalPackedCaller.call(creator,left,right,first,second,shift,mask,low,high);
        if(route==2)try{return (Target)Creator.class.getMethod("make",Object.class,Object.class,float.class,float.class,int.class,long.class,int.class,int.class).invoke(creator,left,right,first,second,shift,mask,low,high);}catch(InvocationTargetException e){throw e.getCause();}
        return (Target)MethodHandles.publicLookup().findVirtual(Creator.class,"make",MethodType.methodType(Target.class,Object.class,Object.class,float.class,float.class,int.class,long.class,int.class,int.class)).invokeExact(creator,left,right,first,second,shift,mask,low,high);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean ordered=Boolean.parseBoolean(args[2]);
        int[] raw={0xc0600000,0x40600000,0x80000000,0x7fc01234,0xffc05678,0x7f800000,0xff800000,1,0x007fffff,0x7f7fffff,0xbf800001,0x3f800001,0,0x80000001,0x7fc09999,0xc0a00000};
        int[] shifts={32,-65,-64,-1,0,1,31,32,63,64,65,Integer.MIN_VALUE,Integer.MAX_VALUE,33,7,-33};
        long[] masks={0xffffffffL,-1L,0,Long.MIN_VALUE,0x123456789abcdefL};
        Trace.failure=new IllegalStateException("owned packed failure");Trace.failAt=mode>=1&&mode<=5?mode:0;
        Creator creator=new Creator();Target previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Object left=mode>=14?null:new String("owned identity"),right=mode==15?null:attempt==0?left:new String("owned identity");
            if(mode==14)right=new Object();
            float first=Float.intBitsToFloat(raw[mode]),second=Float.intBitsToFloat(raw[(mode+5)%raw.length]);
            int shift=shifts[mode],low=mode%3==0?Integer.MIN_VALUE:mode%3==1?Integer.MAX_VALUE:17;
            int high=attempt==0?low:mode%3==0?Integer.MAX_VALUE:Integer.MIN_VALUE;
            long mask=masks[mode%masks.length];Object sentinel=new Object(),seed=new Object();
            Trace.events=0;Trace.seen=sentinel;Trace.result=seed;Trace.tail=37+attempt;int reads=Trace.reads;
            Target result=null;Throwable caught=null;try{result=call(route,creator,left,right,first,second,shift,mask,low,high);}catch(Throwable e){caught=e;}
            String sequence=attempt==0?"12345":mode==1||mode==2?"":"345";
            if(Trace.failAt!=0 && sequence.indexOf((char)('0'+Trace.failAt))>=0)sequence=sequence.substring(0,sequence.indexOf((char)('0'+Trace.failAt))+1);
            require(Trace.events==(sequence.isEmpty()?0:Long.parseLong(sequence)));
            if(mode==1||mode==2)require(attempt==0?caught instanceof ExceptionInInitializerError&&caught.getCause()==Trace.failure:caught instanceof NoClassDefFoundError);
            else if(mode>=3&&mode<=5)require(caught==Trace.failure);
            else {
                boolean choose=ordered?low>=high:left==right;
                long a=(long)Float.floatToRawIntBits(first),b=(long)Float.floatToRawIntBits(second);
                long expected=choose?((a<<shift)|(b&mask)):((b<<shift)^(a&mask));
                require(caught==null && result!=null && result!=previous && result.seed==seed && result.bits==expected && result.tail==37+attempt);previous=result;
            }
            boolean entered=mode!=1&&mode!=2;require(Trace.reads==reads+(entered?1:0) && Trace.seen==(entered?left:sentinel));
            require(Trace.tail==(mode==1||mode==2||mode==3||mode==4?37+attempt:99));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
