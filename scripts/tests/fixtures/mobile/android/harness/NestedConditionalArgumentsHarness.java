import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class NestedConditionalArgumentsHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned nested conditional arguments"); }
    static Outer call(int route,Input first,Input second) throws Throwable {
        try {
            if(route==0)return Creator.make(first,second);
            if(route==1)return NestedConditionalArgumentsCaller.call(first,second);
            if(route==2)return (Outer)Creator.class.getMethod("make",Input.class,Input.class).invoke(null,first,second);
            return (Outer)MethodHandles.publicLookup().findStatic(Creator.class,"make",
                MethodType.methodType(Outer.class,Input.class,Input.class)).invokeExact(first,second);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),branch=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Object a=new Object(),b=new Object(),c=new Object(),d=new Object();
        Input primary=new Input((branch&1)==0?a:null,(branch&2)==0?b:null);
        Input fallback=new Input(c,d);
        Trace.failure=new RuntimeException("owned preparation failure");Trace.failAt=mode>=1&&mode<=8?mode:0;
        Object sentinel=new Object();
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.inner=Trace.outer=sentinel;
            primary.firstCalls=primary.secondCalls=fallback.firstCalls=fallback.secondCalls=0;
            Throwable failure=null;Outer actual=null;
            try { actual=call(route,mode==9?null:primary,mode==10?null:fallback); } catch(Throwable error) { failure=error; }
            String steps=attempt==0?"12":"";
            if((mode==1||mode==2)&&attempt!=0)steps="";
            else if(mode==1)steps="1";
            else if(mode==2)steps="12";
            else if(mode!=9) {
                steps+="3";
                if(mode!=3) {
                    if(mode!=10||(branch&1)==0)steps+="3";
                    if(mode!=10||(branch&1)==0) {
                        steps+="4";
                        if(mode!=4) {
                            if(mode!=10||(branch&2)==0)steps+="4";
                            if(mode!=10||(branch&2)==0) {
                                steps+="5";if(mode!=5) {steps+="6";if(mode!=6) {steps+="7";if(mode!=7)steps+="8";}}
                            }
                        }
                    }
                }
            }
            long expected=0;for(char digit:steps.toCharArray())expected=expected*10+(digit-'0');
            require(Trace.events==expected);
            if(mode==1||mode==2) {
                if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if(mode>=3&&mode<=8)require(failure==Trace.failure);
            else if(mode==9||mode==10&&branch!=0)require(failure instanceof NullPointerException);
            else require(failure==null&&actual==Trace.outer);
            int threes=0,fours=0;for(char digit:steps.toCharArray()) {if(digit=='3')threes++;if(digit=='4')fours++;}
            require(primary.firstCalls==((branch&1)==0?threes:Math.min(threes,1)));
            require(primary.secondCalls==((branch&2)==0?fours:Math.min(fours,1)));
            require(fallback.firstCalls==((branch&1)!=0&&threes==2?1:0));
            require(fallback.secondCalls==((branch&2)!=0&&fours==2?1:0));
            boolean inner=steps.indexOf('6')>=0,outer=steps.indexOf('8')>=0;
            require((Trace.inner!=sentinel)==inner && (Trace.outer!=sentinel)==outer);
            if(inner) {
                Inner captured=(Inner)Trace.inner;
                require(captured.first==((branch&1)==0?a:c)&&captured.second==((branch&2)==0?b:d));
                if(outer)require(((Outer)Trace.outer).value==captured);
            }
            System.out.println("route="+route+",branch="+branch+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
