import fixture.Child;
import fixture.Service;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ClosedBoundSuperHarness {
    private static void require(boolean value) {if(!value)throw new AssertionError("owned closed bound super");}
    private static Child call(int route,ClassLoader loader,Service manager) throws Throwable {
        if(route==0)return new Child(loader,manager);
        if(route==1)return ClosedBoundSuperCaller.create(loader,manager);
        if(route==2) {try{return Child.class.getConstructor(ClassLoader.class,Service.class).newInstance(loader,manager);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,ClassLoader.class,Service.class)).invokeExact(loader,manager);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);require(operation==0);
        final ClassLoader loader=mode==11?null:new ClassLoader(null){};
        final RuntimeException parentFailure=new RuntimeException("owned parent failure"),snapshotFailure=new RuntimeException("owned snapshot failure");
        final Error parentError=new Error("owned parent error"),snapshotError=new Error("owned snapshot error");
        final Object desired=mode==8?new Object():mode==9?null:new String("owned opaque snapshot");
        final Service observed=new Service(){public Object snapshot(){
            require(Trace.events==1324&&Trace.partial instanceof Child&&((Child)Trace.partial).loader==loader);Trace.calls++;Trace.hit(5);Trace.snapshotAction.run();return Trace.result;
        }};
        final Service manager=mode==10?null:observed;
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==1?1:mode==4?2:mode==5?4:0;
        Object previousPartial=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),oldPartial=previousPartial;Trace.events=0;Trace.partial=Trace.result=sentinel;int parents=Trace.parents,calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){
                require(Trace.events==1&&Trace.partial instanceof Child&&Trace.partial!=oldPartial);Child partial=(Child)Trace.partial;require(partial.manager==manager&&partial.loader==null);Trace.hit(3);
                if(mode==2)throw parentFailure;if(mode==3)throw parentError;
            }};
            Trace.snapshotAction=new Runnable(){public void run(){require(Trace.events==13245);Trace.hit(6);Trace.result=desired;if(mode==6)throw snapshotFailure;if(mode==7)throw snapshotError;}};
            Child actual=null;Object captured=sentinel;int length=42;Throwable failure=null;
            try{actual=call(route,loader,manager);captured=actual.capture();length=((String)captured).length();}catch(Throwable error){failure=error;}
            boolean constructorFailed=mode>=1&&mode<=5;long events=mode==1?1:mode==2||mode==3?13:mode==4?132:mode==5||mode==10?1324:132456;
            require(Trace.events==events&&Trace.parents==parents+1&&Trace.calls==calls+(constructorFailed||mode==10?0:1));
            Throwable expected=mode==1||mode==4||mode==5?Trace.failure:mode==2?parentFailure:mode==3?parentError:mode==6?snapshotFailure:mode==7?snapshotError:null;
            if(expected!=null)require(failure==expected&&actual==(constructorFailed?null:Trace.partial)&&length==42&&captured==sentinel);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==Trace.partial&&length==42&&captured==desired);
            else if(mode==9||mode==10)require(failure instanceof NullPointerException&&actual==Trace.partial&&length==42&&captured==(mode==10?sentinel:desired));
            else require(failure==null&&actual==Trace.partial&&captured==desired&&length==((String)desired).length());
            if(mode==1)require(Trace.partial==sentinel);else {Child partial=(Child)Trace.partial;require(partial!=previousPartial&&partial.manager==manager&&partial.loader==(mode>=2&&mode<=4?null:loader));previousPartial=partial;}
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
