import fixture.*;
public final class BoundedTokenElementHarness {
    static int mode,route,step;static long events;
    static Context original,decoy;static Proxy factory;static Token token;static String name;
    static Bound selected,late;static RuntimeException failure;
    static void check(boolean value){if(!value)throw new AssertionError("owned bounded token element mismatch");}
    static final class Opaque extends Bound {}
    static final class Implementation extends Proxy {
        protected <T extends Bound> T create(String name,Token<T> token,Context context){
            Trace.hit(4,this,name,token,context,null);if(mode==5)throw failure;
            return (T)(Bound)context.reply;
        }
    }
    static final class Observer implements Trace.Observer {
        public void observe(int phase,Factory receiver,String n,Token t,Context context,Bound result){
            int[] order={0,1,4,2,3};check(step<order.length&&phase==order[step++]);events=events*10+phase;
            check(receiver==factory&&n==name&&t==token);
            check(context==(phase==1||phase==4||phase==2?original:null));
            if(phase==2||phase==3&&route==0)check(result==(mode==7?null:selected));else check(result==null);
            if(phase==1)Trace.current=decoy;
            if(phase==4&&original!=null)original.reply=mode==14?new Object():mode==7?null:selected;
            if(phase==2&&original!=null)original.reply=late;
            if(phase==3&&mode==11)selected.value="after";
            if(mode==8&&phase==1||mode==9&&phase==2||mode==10&&phase==3)throw failure;
        }
    }
    public static void main(String[] args){
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++){
            step=0;events=0;original=mode==4?null:new Context();decoy=new Context();
            selected=new Opaque();selected.value=mode==6||mode==10?new Object():"selected";
            late=new Opaque();late.value="late";if(original!=null){Bound early=new Opaque();early.value="early";original.reply=early;}decoy.reply=late;
            factory=mode==1?null:new Implementation();name=mode==13?null:"owned";token=mode==12?null:new Token();failure=new RuntimeException("owned failure");
            Trace.current=original;Trace.observer=new Observer();Throwable caught=null;String result=null;
            try{if(route==0)result=factory.run(mode==2?new Object():name,mode==3?new Object():token);else factory.ignore(mode==2?new Object():name,mode==3?new Object():token);}catch(Throwable value){caught=value;}
            if(mode==1)check(caught instanceof NullPointerException);
            else if(mode==2||mode==3||mode==14||mode==6&&route==0)check(caught instanceof ClassCastException);
            else if(mode==4||mode==7&&route==0)check(caught instanceof NullPointerException);
            else if(mode==5||mode==8||mode==9||mode==10)check(caught==failure);
            else check(caught==null&&(route==1||result.equals(mode==11?"after":"selected")));
            int expected=mode==1||mode==2||mode==3?0:mode==8?2:mode==4||mode==5||mode==14?3:mode==9?4:5;
            check(step==expected&&events==(expected==0?0:expected==2?1:expected==3?14:expected==4?142:1423));
        }
        System.out.println("attempts=2,bounded-token-element=matched");
    }
}
