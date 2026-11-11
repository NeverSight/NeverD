package fixture;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PrivateNullaryCarrierHarness {
    static void require(boolean condition) {
        if(!condition)throw new AssertionError("owned private nullary carrier observer");
    }
    static Object call(int route,boolean checked) throws Throwable {
        if(route==0)return checked?Owner.text():Owner.call();
        if(route==1)return PrivateNullaryCarrierCaller.call(checked);
        if(route==2) {
            try {return Owner.class.getMethod(checked?"text":"call").invoke(null);}
            catch(InvocationTargetException error){throw error.getCause();}
        }
        if(checked)return (String)MethodHandles.lookup().findStatic(Owner.class,"text",MethodType.methodType(String.class)).invokeExact();
        return (Box<?>)MethodHandles.lookup().findStatic(Owner.class,"call",MethodType.methodType(Box.class)).invokeExact();
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        boolean checked=mode>=13;
        int point=mode>=1&&mode<=6?mode:mode==17?2:mode==18?4:mode==19?5:mode==20?6:mode==21?7:mode==23?3:0;
        Trace.failAt=point;Trace.failure=new RuntimeException("owned factory failure");
        Object untouched=new Object();Box<?> previous=null;
        Trace.seed=mode==10||mode==15?null:mode==14?new Object():new String("original value");
        Trace.values=mode==11?null:new float[]{Float.intBitsToFloat(0x7fc12345),-0.0f,Float.POSITIVE_INFINITY};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.partial=Trace.allocated=untouched;
            Trace.override=mode==7||mode==8||mode==16||(mode==9||mode==22)&&attempt==1;
            Trace.result=mode==8?untouched:mode==9||mode==22?previous:null;
            Object result=null;Throwable failure=null;
            try {result=call(route,checked);}catch(Throwable error){failure=error;}
            boolean entered=point!=1;
            boolean allocationAttempt=entered&&point!=2;
            boolean constructor=allocationAttempt&&point!=3;
            boolean fields=constructor&&point!=4;
            boolean allocated=fields&&point!=5;
            boolean returned=allocated&&point!=6&&mode!=8;
            boolean getter=checked&&returned&&mode!=16;
            String expected=attempt==0?"1":"";
            if(entered)expected+="2";
            if(allocationAttempt&&attempt==0)expected+="3";
            if(constructor)expected+="4";if(fields)expected+="5";if(allocated)expected+="6";if(getter)expected+="7";
            require(Trace.events==(expected.isEmpty()?0:Long.parseLong(expected)));
            if(point==1||point==3) {
                if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if(point!=0)require(failure==Trace.failure);
            else if(mode==8||mode==14)require(failure instanceof ClassCastException);
            else if(mode==16)require(failure instanceof NullPointerException);
            else {
                Object expectedBox=Trace.override?Trace.result:Trace.allocated;
                require(failure==null&&result==(checked?((Box<?>)expectedBox).value:expectedBox));
            }
            require((Trace.partial!=untouched)==constructor&&(Trace.allocated!=untouched)==allocated);
            if(constructor) {
                Box<?> partial=(Box<?>)Trace.partial;
                require(partial.value==(fields?Trace.seed:null)&&partial.values==(fields?Trace.values:null));
                if(allocated)require(Trace.allocated==partial&&partial!=previous);
            }
            if(allocated)previous=(Box<?>)Trace.allocated;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
