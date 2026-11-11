import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class MultipleFinalExitsHarness {
    static void require(boolean value) { if(!value)throw new AssertionError("owned multiple final exits"); }
    static Core create(int route,long a,long b) throws Throwable {
        if(route==0)return new Core(a,b);
        if(route==1)return MultipleFinalExitsCaller.create(a,b);
        if(route==2)try{return Core.class.getConstructor(long.class,long.class).newInstance(a,b);}catch(InvocationTargetException e){throw e.getCause();}
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,long.class,long.class)).invokeExact(a,b);
    }
    static long read(int route) throws Throwable {
        if(route==0)return StaticCore.second;
        if(route==1)return MultipleFinalExitsCaller.read();
        if(route==2)return StaticCore.class.getField("second").getLong(null);
        return (long)MethodHandles.publicLookup().findStaticGetter(StaticCore.class,"second",long.class).invokeExact();
    }
    public static void main(String[] args) throws Throwable {
        boolean statics=Boolean.parseBoolean(args[0]);int route=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Trace.mode=mode==4?1:mode==5?2:mode==6?3:mode==7?0:mode;
        Trace.failure1=mode==4?null:new IllegalArgumentException("owned first exit");
        Trace.failure2=mode==5?null:new IllegalStateException("owned second exit");
        Trace.failure3=mode==6?null:new UnsupportedOperationException("owned third exit");
        Trace.observerFailure=new IllegalArgumentException("owned observer exit");Trace.failAt=mode==7?2:0;
        Core previous=null;long initialA=Long.MIN_VALUE+1,initialB=Long.MAX_VALUE-7;
        for(int attempt=0;attempt<2;attempt++) {
            long a=initialA+attempt,b=initialB-attempt;Trace.a=a;Trace.b=b;Trace.events=0;Trace.escaped=null;
            int[] observations={0};
            Trace.observer=event->{
                observations[0]++;long first=statics?StaticCore.first:Trace.escaped.first;
                long second=statics?StaticCore.second:Trace.escaped.second;
                require(first==(event==1?0:a));
                require(second==(event==1||event==2||event==5?0:b));
            };
            Core result=null;long value=0;Throwable caught=null;
            try{if(statics)value=read(route);else result=create(route,a,b);}catch(Throwable e){caught=e;}
            String sequence=mode==0?"1234":mode==1||mode==4?"125":mode==2||mode==5?"1236":mode==3||mode==6?"1237":"12";
            if(statics&&attempt==1)sequence="";
            require(Trace.events==(sequence.isEmpty()?0:Long.parseLong(sequence)) && observations[0]==sequence.length());
            if(mode==0) {
                require(caught==null);
                if(statics)require(value==initialB && StaticCore.first==initialA);
                else {require(result==Trace.escaped && result!=previous && result.first==a && result.second==b);previous=result;}
            } else {
                if(statics && attempt==1)require(caught instanceof NoClassDefFoundError);
                else {
                    Throwable actual=caught;
                    if(statics){require(caught instanceof ExceptionInInitializerError);actual=caught.getCause();}
                    Throwable expected=mode==1?Trace.failure1:mode==2?Trace.failure2:mode==3?Trace.failure3:mode==7?Trace.observerFailure:null;
                    require(expected==null?actual instanceof NullPointerException:actual==expected);
                }
                if(!statics)require(Trace.escaped!=null && Trace.escaped.first==a && Trace.escaped.second==(mode==1||mode==4||mode==7?0:b));
            }
            System.out.println("static="+statics+",route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
