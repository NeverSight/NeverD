import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConstructorChoiceHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned constructor choice");}
    static Widget call(int route,Widget peer) throws Throwable {
        if(route==0)return Core.make(peer);
        if(route==1)return ConstructorChoiceCaller.call(peer);
        if(route==2)try{return (Widget)Core.class.getMethod("make",Widget.class).invoke(null,peer);}catch(InvocationTargetException e){throw e.getCause();}
        return (Widget)MethodHandles.publicLookup().findStatic(Core.class,"make",MethodType.methodType(Widget.class,Widget.class)).invokeExact(peer);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean present=Boolean.parseBoolean(args[2]),copyOnNull=Boolean.parseBoolean(args[3]);
        require(Modifier.isPrivate(Core.class.getDeclaredConstructor().getModifiers()));
        Method make=Core.class.getMethod("make",Widget.class);require(Modifier.isStatic(make.getModifiers())&&!make.isBridge()&&!make.isSynthetic());
        require(Widget.class.getDeclaredConstructors().length==2&&Modifier.isFinal(Widget.class.getField("kind").getModifiers())&&Modifier.isFinal(Widget.class.getField("source").getModifiers()));
        Trace.failAt=0;Trace.observer=null;Widget peer=present?new Widget():null;
        RuntimeException primary=new IllegalStateException("owned primary"),observerFailure=new IllegalArgumentException("owned observer"),handlerFailure=new UnsupportedOperationException("owned handler");
        Error observerError=new AssertionError("owned error");Widget previous=null;
        final boolean copying=present!=copyOnNull;final int kind=copying?2:1,first=copying?5:2,mid=first+1,last=first+2;
        for(int attempt=0;attempt<2;attempt++) {
            final int seed=mode==11?Integer.MAX_VALUE:mode==12?Integer.MIN_VALUE:mode==13?0x7fc01234:-19-attempt,next=31+attempt;
            Object sentinel=new Object();Trace.events=0;Trace.constructors=0;Trace.seed=seed;Trace.next=next;Trace.failure=primary;Trace.caught=null;Trace.current=null;Trace.received=sentinel;
            Trace.failAt=mode>=1&&mode<=7?mode:mode==8?first:0;
            Trace.observer=event->{
                if(event==first)require(Trace.current!=null&&Trace.current.kind==0&&Trace.current.source==null&&Trace.current.value==0);
                if(event==mid){require(Trace.current.kind==kind&&Trace.current.source==null&&Trace.current.value==0);if(mode==9)Trace.seed=next;if(mode==15)throw observerError;}
                if(event==last){require(Trace.current.kind==kind&&Trace.current.source==(copying?peer:null)&&Trace.current.value==(mode==9?next:seed));if(mode==10)Trace.seed=next;if(mode==14)throw observerFailure;}
                if(event==8&&mode==8)throw handlerFailure;
            };
            Widget result=null;Throwable raw=null;try{result=call(route,peer);}catch(Throwable e){raw=e;}
            long events=0;boolean entered=false,kindStored=false,valueStored=false;Throwable problem=null,expectedRaw=null,expectedCatch=null;
            boolean failedClass=!present&&mode==1;
            if(!present&&attempt==0)events=1;
            if(failedClass){if(attempt==0){require(raw instanceof ExceptionInInitializerError&&raw.getCause()==primary);}else require(raw instanceof NoClassDefFoundError);}
            else {
                for(int event=first;event<=last;event++) {
                    events=events*10+event;
                    if(event==first)entered=true;if(event==mid)kindStored=true;if(event==last)valueStored=true;
                    if(mode==15&&event==mid)problem=observerError;
                    else if(mode==14&&event==last)problem=observerFailure;
                    else if(event==Trace.failAt)problem=primary;
                    if(problem!=null)break;
                }
                if(problem instanceof Exception){expectedCatch=problem;events=events*10+8;expectedRaw=mode==8?handlerFailure:null;}
                else if(problem!=null)expectedRaw=problem;
                require(raw==expectedRaw);
                if(problem==null)require(result!=null&&result==Trace.current&&result!=previous);
                else require(result==null);
            }
            require(Trace.events==events&&Trace.constructors==(entered?1:0)&&Trace.caught==expectedCatch);
            require((Trace.current!=null)==entered&&Trace.received==(entered?(copying?peer:null):sentinel));
            if(entered)require(Trace.current.kind==(kindStored?kind:0)&&Trace.current.source==(valueStored&&copying?peer:null)&&Trace.current.value==(valueStored?(mode==9?next:seed):0));
            int current=(mode==9&&kindStored||mode==10&&valueStored)?next:seed;require(Trace.seed==current);
            if(result!=null)previous=result;
            System.out.println("route="+route+",mode="+mode+",peer="+present+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
