import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PrivateStaticFieldNamesHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned private static fields");}
    static Callback call(int route,Callback next,long bits) throws Throwable {
        if(route==0)return Client.call(next,bits);
        if(route==1)return PrivateStaticFieldNamesCaller.call(next,bits);
        if(route==2)try{return (Callback)Provider.class.getMethod("exchange",Callback.class,long.class).invoke(null,next,bits);}catch(InvocationTargetException e){throw e.getCause();}
        return (Callback)MethodHandles.publicLookup().findStatic(Provider.class,"exchange",MethodType.methodType(Callback.class,Callback.class,long.class)).invokeExact(next,bits);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        RuntimeException failure=new IllegalStateException("owned original failure");Trace.failure=failure;
        Object value=mode==4?null:new Object(),output=new Object();int[] calls={0};
        Callback<Object,Object> initial=arg->arg;
        Callback<Object,Object> stored=mode==1?null:initial;
        long bits=17;Trace.initial=stored;Trace.initialBits=bits;
        int stop=mode>=7&&mode<=9?mode-6:mode>=10?mode-6:0;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.seen=null;Trace.seenInput=null;Trace.failAt=stop;
            Callback<Object,Object> replacement=arg->{require(arg==value);calls[0]++;Trace.seenInput=arg;Trace.hit(9);if(mode==3)throw failure;return output;};
            Callback<Object,Object> next=mode==2?null:replacement;
            long nextBits=mode==5?Long.MIN_VALUE:mode==6?Long.MAX_VALUE:41+attempt;
            int before=calls[0];Callback previous=stored,old=null;Throwable caught=null;Object result=null;
            try { old=call(route,next,nextBits);result=Provider.invoke(value); }catch(Throwable error){caught=error;}
            long events=0;
            if(stop>=1&&stop<=3) {
                if(attempt==0) {
                    for(int event=1;event<=stop;event++)events=events*10+event;
                    require(caught instanceof ExceptionInInitializerError&&caught.getCause()==failure);
                } else require(caught instanceof NoClassDefFoundError);
                require(Trace.events==events&&Trace.seen==null&&calls[0]==before);
            } else {
                if(attempt==0)events=123;
                for(int event=4;event<=7;event++){events=events*10+event;if(event==stop)break;}
                if(stop==0){events=events*10+8;if(next!=null)events=events*10+9;}
                if(stop==0||stop>=6)stored=next;
                if(stop==0||stop>=7)bits=nextBits;
                require(Trace.events==events&&Trace.seen==(stop==4?null:previous));
                require(Provider.peek()==stored&&Provider.bits()==bits);
                require(calls[0]==before+(stop==0&&next!=null?1:0));
                if(stop!=0)require(caught==failure);
                else {
                    require(old==previous);
                    if(mode==2)require(caught instanceof NullPointerException);
                    else if(mode==3)require(caught==failure&&Trace.seenInput==value);
                    else require(caught==null&&result==output&&Trace.seenInput==value);
                }
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
