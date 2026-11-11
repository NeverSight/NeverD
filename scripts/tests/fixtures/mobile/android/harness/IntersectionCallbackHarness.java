import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class IntersectionCallbackHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned intersection callback"); }
    static void call(int route,Host host) throws Throwable {
        try {
            if(route==0)host.run();
            else if(route==1)IntersectionCallbackCaller.call(host);
            else if(route==2)Host.class.getMethod("run").invoke(host);
            else MethodHandles.publicLookup().findVirtual(Host.class,"run",MethodType.methodType(void.class)).invoke(host);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getDeclaredMethod("observe",Base.class,Callback.class);
        TypeVariable<?> formal=method.getTypeParameters()[0];
        require(formal.getBounds().length==2&&formal.getBounds()[0]==Base.class&&formal.getBounds()[1]==Observer.class);
        require(method.getGenericParameterTypes()[0].equals(formal));
        require(((ParameterizedType)method.getGenericParameterTypes()[1]).getActualTypeArguments()[0]==Unit.class);
        Trace.failure=new RuntimeException("owned callback failure");Trace.failAt=mode>=1&&mode<=6?mode:mode==7?9:0;
        Unit unit=new Unit();int[] calls={0};
        Callback<Unit> callback=mode==8?null:()->{calls[0]++;Trace.hit(6);if(mode==10)Trace.nodeCalls++;return mode==9?null:unit;};
        Host host=new Host(callback);Object sentinel=new Object();
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.node=Trace.callback=sentinel;
            int oldNode=Trace.nodeCalls,oldGetter=Trace.getterCalls,oldCalls=calls[0];Throwable failure=null;
            try { call(route,host); } catch(Throwable error) { failure=error; }
            String steps="5";
            boolean poisoned=mode==7&&attempt!=0;
            if(mode!=5&&!poisoned) {
                if(attempt==0)steps+="9";
                if(mode!=7) {
                    steps+="1";if(mode!=1) {steps+="4";if(mode!=4) {steps+="2";if(mode!=2&&mode!=8) {steps+="6";if(mode!=6)steps+="3";}}}
                }
            }
            long expected=0;for(char digit:steps.toCharArray())expected=expected*10+(digit-'0');
            require(Trace.events==expected && Trace.getterCalls==oldGetter+1);
            boolean observed=steps.indexOf('4')>=0,called=steps.indexOf('6')>=0;
            require(Trace.nodeCalls==oldNode+(observed?1:0)+(mode==10&&called?1:0)&&calls[0]==oldCalls+(called?1:0));
            require(Trace.node==(observed?host:sentinel));
            boolean callbackWritten=steps.indexOf('2')>=0&&mode!=2;
            require(Trace.callback==(callbackWritten?callback:sentinel));
            if(mode>=1&&mode<=6)require(failure==Trace.failure);
            else if(mode==7)require(attempt==0?failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure:failure instanceof NoClassDefFoundError);
            else if(mode==8)require(failure instanceof NullPointerException);
            else require(failure==null);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
