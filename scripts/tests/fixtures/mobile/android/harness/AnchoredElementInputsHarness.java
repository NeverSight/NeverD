import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class AnchoredElementInputsHarness {
    static void require(boolean condition) {
        if(!condition)throw new AssertionError("owned anchored element input observer");
    }
    static Object call(Cell<?> cell,float offset,float limit,Fn<?,?> callback,Producer<?> producer,int route,boolean checked) throws Throwable {
        if(route==0)return checked?Owner.text(cell,offset,limit,callback,producer):Owner.call(cell,offset,limit,callback,producer);
        if(route==1)return AnchoredElementInputsCaller.call(cell,offset,limit,callback,producer,checked);
        if(route==2) {
            try {return Owner.class.getMethod(checked?"text":"call",Cell.class,float.class,float.class,Fn.class,Producer.class)
                    .invoke(null,cell,offset,limit,callback,producer);}
            catch(InvocationTargetException error){throw error.getCause();}
        }
        if(checked)return (String)MethodHandles.lookup().findStatic(Owner.class,"text",
                MethodType.methodType(String.class,Cell.class,float.class,float.class,Fn.class,Producer.class))
                .invokeExact((Cell)cell,offset,limit,(Fn)callback,(Producer)producer);
        return (Object)MethodHandles.lookup().findStatic(Owner.class,"call",
                MethodType.methodType(Object.class,Cell.class,float.class,float.class,Fn.class,Producer.class))
                .invokeExact((Cell)cell,offset,limit,(Fn)callback,(Producer)producer);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Trace.failure=new RuntimeException("owned private generic failure");Trace.failAt=mode>=1&&mode<=7?mode:0;
        Error callbackFailure=new AssertionError("owned callback failure");
        Object untouched=new Object();int[] counts={0,0};
        Label produced=mode==11?null:new Label(),mapped=mode==12?null:new Label();
        Cell<Object> original=new Cell<Object>();original.first=mode==25||mode==30?null:new String("first");original.second=new Object();
        Cell<?> cell=mode==8?null:original;
        float offset=mode==17?Float.NaN:mode==18?-0.0f:mode==19?Float.NEGATIVE_INFINITY:
                mode==20?Float.POSITIVE_INFINITY:mode==21?Float.MIN_VALUE:mode==22?-1.0f:mode==29?3.0f:
                mode==31?Float.intBitsToFloat(0x7fc12345):1.0f;
        float limit=mode==18||mode==21?0.0f:mode==19?Float.POSITIVE_INFINITY:mode==20?Float.NEGATIVE_INFINITY:
                mode==22?Float.NaN:mode==31?Float.intBitsToFloat(0xffc54321):2.0f;
        Trace.override=mode>=23&&mode<=24||mode>=26&&mode<=28;
        Trace.result=mode==24||mode==28?null:mode==26?new String("override"):new int[]{17};
        Producer<Label> producer=()->{counts[0]++;Trace.hit(3);if(mode==13)throw callbackFailure;return produced;};
        Fn<Label,Label> callback=incoming->{counts[1]++;Trace.hit(5);require(incoming==produced);if(mode==14)throw callbackFailure;return mapped;};
        Producer<?> actualProducer=mode==9?null:mode==15?(Producer<Object>)()->{counts[0]++;Trace.hit(3);return untouched;}:producer;
        Fn<?,?> actualCallback=mode==10?null:mode==16?(Fn<Label,Object>)incoming->{counts[1]++;Trace.hit(5);require(incoming==produced);return untouched;}:callback;
        boolean checked=mode>=26&&mode<=30;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.cell=Trace.produced=Trace.mapped=Trace.selected=untouched;
            Trace.offset=Trace.limit=31.5f;int p=counts[0],c=counts[1];Object result=null;Throwable failure=null;
            try {result=call(cell,offset,limit,actualCallback,actualProducer,route,checked);}
            catch(Throwable error){failure=error;}
            boolean entered=mode!=1;
            boolean gotProducer=entered&&mode!=2&&mode!=9;
            boolean storedProduced=gotProducer&&mode!=3&&mode!=13&&mode!=15;
            boolean gotCallback=storedProduced&&mode!=4&&mode!=10;
            boolean storedMapped=gotCallback&&mode!=5&&mode!=14&&mode!=16;
            boolean storedSelected=storedMapped&&mode!=6&&mode!=8;
            Object chosen=offset<limit?original.first:original.second;
            String expected=attempt==0?"1":"";
            if(entered)expected+="2";if(gotProducer)expected+="3";if(storedProduced)expected+="4";
            if(gotCallback)expected+="5";if(storedMapped)expected+="6";if(storedSelected)expected+="7";
            require(Trace.events==(expected.isEmpty()?0:Long.parseLong(expected)));
            if(mode==1) {
                if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if(mode>=2&&mode<=7)require(failure==Trace.failure);
            else if(mode>=8&&mode<=10)require(failure instanceof NullPointerException);
            else if(mode==13||mode==14)require(failure==callbackFailure);
            else if(mode==15||mode==16||mode==27||mode==29)require(failure instanceof ClassCastException);
            else require(failure==null&&result==(Trace.override?Trace.result:chosen));
            require(Trace.cell==(entered?cell:untouched));
            require(Float.floatToRawIntBits(Trace.offset)==Float.floatToRawIntBits(entered?offset:31.5f));
            require(Float.floatToRawIntBits(Trace.limit)==Float.floatToRawIntBits(entered?limit:31.5f));
            require(Trace.produced==(storedProduced?produced:untouched)&&Trace.mapped==(storedMapped?mapped:untouched));
            require(Trace.selected==(storedSelected?chosen:untouched));
            require(counts[0]==p+(gotProducer?1:0)&&counts[1]==c+(gotCallback?1:0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
