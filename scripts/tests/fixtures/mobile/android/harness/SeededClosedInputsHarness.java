import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class SeededClosedInputsHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned seeded closed-input observer");
    }
    static Box<?> call(Object seed, Box<?> input, Fn<?,?> closed,
            Fn<?,?> callback, Producer<?> producer, int route) throws Throwable {
        if(route==0) return Client.call(seed,input,closed,callback,producer);
        if(route==1) return SeededClosedInputsCaller.call(seed,input,closed,callback,producer);
        if(route==2) {
            try {return (Box<?>)Client.class.getMethod("call",Object.class,Box.class,Fn.class,Fn.class,Producer.class)
                    .invoke(null,seed,input,closed,callback,producer);}
            catch(InvocationTargetException error) {throw error.getCause();}
        }
        return (Box<?>)MethodHandles.lookup().findStatic(Client.class,"call",
                MethodType.methodType(Box.class,Object.class,Box.class,Fn.class,Fn.class,Producer.class))
                .invokeExact(seed,(Box)input,(Fn)closed,(Fn)callback,(Producer)producer);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Trace.failure=new RuntimeException("owned factory failure");
        Trace.failAt=mode>=1&&mode<=8?mode:0;
        Error callbackFailure=new AssertionError("owned callback failure");
        Object untouched=new Object();
        int[] value={31},counts={0,0,0};
        Object seed=mode==12?"late incompatible seed":mode==13?null:value;
        Label produced=mode==15?null:new Label();
        Label first=mode==16?null:new Label();
        Label second=mode==17?null:new Label();
        Box<Object> inputBox=new Box<Object>(); inputBox.value=mode==26?untouched:seed;
        Box<?> input=mode==14?null:inputBox;
        Box<Object> output=new Box<Object>(); output.value=mode==19?untouched:second;
        Trace.box=mode==18?null:mode==27?inputBox:output;
        Producer<Label> producer=()->{
            counts[0]++;Trace.hit(3);
            if(mode==20)throw callbackFailure;
            return produced;
        };
        Fn<Label,Label> closed=incoming->{
            counts[1]++;Trace.hit(5);require(incoming==produced);
            if(mode==21)throw callbackFailure;
            return first;
        };
        Fn<int[],Label> callback=incoming->{
            counts[2]++;Trace.hit(7);require(incoming==seed);
            if(mode==22)throw callbackFailure;
            if(incoming!=null)incoming[0]++;
            return second;
        };
        Producer<?> actualProducer=mode==9?null:mode==23?(Producer<Object>)()->{counts[0]++;Trace.hit(3);return untouched;}:producer;
        Fn<?,?> actualClosed=mode==10?null:mode==24?(Fn<Label,Object>)incoming->{counts[1]++;Trace.hit(5);require(incoming==produced);return untouched;}:closed;
        Fn<?,?> actualCallback=mode==11?null:mode==25?(Fn<Object,Object>)incoming->{counts[2]++;Trace.hit(7);require(incoming==seed);return untouched;}:callback;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.seed=Trace.input=Trace.produced=Trace.first=Trace.second=untouched;
            int p=counts[0],c=counts[1],m=counts[2],oldValue=value[0];
            Throwable failure=null;Box<?> result=null;
            try {result=call(seed,input,actualClosed,actualCallback,actualProducer,route);}
            catch(Throwable error){failure=error;}
            boolean entered=mode!=1;
            boolean gotProducer=entered&&mode!=2&&mode!=9;
            boolean storedProduced=gotProducer&&mode!=3&&mode!=20&&mode!=23;
            boolean gotClosed=storedProduced&&mode!=4&&mode!=10;
            boolean storedFirst=gotClosed&&mode!=5&&mode!=21&&mode!=24;
            boolean gotMixed=storedFirst&&mode!=6&&mode!=11&&mode!=12;
            boolean storedSecond=gotMixed&&mode!=7&&mode!=22&&mode!=25;
            String expected=attempt==0?"1":"";
            if(entered)expected+="2";
            if(gotProducer)expected+="3";
            if(storedProduced)expected+="4";
            if(gotClosed)expected+="5";
            if(storedFirst)expected+="6";
            if(gotMixed)expected+="7";
            if(storedSecond)expected+="8";
            require(Trace.events==(expected.isEmpty()?0:Long.parseLong(expected)));
            if(mode==1) {
                if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if(mode>=2&&mode<=8)require(failure==Trace.failure);
            else if(mode>=9&&mode<=11)require(failure instanceof NullPointerException);
            else if(mode==12||mode>=23&&mode<=25)require(failure instanceof ClassCastException);
            else if(mode>=20&&mode<=22)require(failure==callbackFailure);
            else require(failure==null&&result==Trace.box&&(result==null||result.value==(mode==27?inputBox.value:output.value)));
            require(Trace.seed==(entered?seed:untouched)&&Trace.input==(entered?input:untouched));
            require(Trace.produced==(storedProduced?produced:untouched));
            require(Trace.first==(storedFirst?first:untouched)&&Trace.second==(storedSecond?second:untouched));
            require(counts[0]==p+(gotProducer?1:0)&&counts[1]==c+(gotClosed?1:0)&&counts[2]==m+(gotMixed?1:0));
            require(value[0]==oldValue+(storedSecond&&mode!=13?1:0));
            require(inputBox.value==(mode==26?untouched:seed));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
