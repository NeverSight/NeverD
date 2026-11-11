import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class TypedVariadicConstructorHarness {
    static void require(boolean ok) {if(!ok)throw new AssertionError("owned typed variadic constructor");}
    static Target<?> call(int route,int kind,Class<String> type,String[] names) throws Throwable {
        boolean flag=kind==2;
        if(route==0)return kind==0?new Target<String>(type,names):new Target<String>(flag,type,names);
        if(route==1)return kind==0?TypedVariadicConstructorCaller.defaults(type,names):TypedVariadicConstructorCaller.direct(flag,type,names);
        if(route==2) {
            try {return (Target<?>)(kind==0?Target.class.getConstructor(Class.class,String[].class).newInstance(type,names):Target.class.getConstructor(boolean.class,Class.class,String[].class).newInstance(flag,type,names));}
            catch(InvocationTargetException failure) {throw failure.getCause();}
        }
        if(kind==0)return (Target)MethodHandles.publicLookup().findConstructor(Target.class,MethodType.methodType(void.class,Class.class,String[].class)).invokeExact((Class)type,names);
        return (Target)MethodHandles.publicLookup().findConstructor(Target.class,MethodType.methodType(void.class,boolean.class,Class.class,String[].class)).invokeExact(flag,(Class)type,names);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Class<String> type=mode==8?null:mode==9?(Class)Integer.class:String.class;
        final String initial=new String("owned initial"),changed=new String("owned changed");
        final String[] names=mode==7?null:mode==10?new String[0]:new String[]{initial,mode==11?initial:new String("owned second")};
        final RuntimeException callbackFailure=new RuntimeException("owned callback failure");final Error errorFailure=new Error("owned callback error");
        final Object desired=mode==5?null:mode==6?Integer.valueOf(23):new String("owned result");
        final boolean flag=kind==2;Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==1?1:mode==2?2:0;
        Object previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),old=previous;Trace.events=0;Trace.partial=sentinel;Trace.result=sentinel;int created=Trace.created;
            if(names!=null&&names.length>0)names[0]=initial;
            Trace.action=new Runnable() {public void run() {
                require(Trace.events==1&&Trace.partial instanceof Target&&Trace.partial!=old);
                Target<?> partial=(Target<?>)Trace.partial;
                require(partial.flag==flag&&partial.type==type&&partial.names==names);
                if(names!=null&&names.length>0)require(names[0]==initial);
                Trace.hit(3);Trace.result=desired;if(names!=null&&names.length>0)names[0]=changed;
                if(mode==3)throw callbackFailure;if(mode==4)throw errorFailure;
            }};
            Target<?> actual=null;Object value=sentinel;Throwable failure=null;
            try {actual=call(route,kind,type,names);value=(String)actual.type.cast(Trace.result);}
            catch(Throwable error) {failure=error;}
            boolean early=mode==1||mode==2||mode==3||mode==4;
            require(Trace.events==(mode==1?1:mode==3||mode==4?13:132)&&Trace.created==created+1);
            Throwable expected=mode==1||mode==2?Trace.failure:mode==3?callbackFailure:mode==4?errorFailure:null;
            if(expected!=null)require(failure==expected&&actual==null&&value==sentinel);
            else if(mode==8)require(failure instanceof NullPointerException&&actual==Trace.partial);
            else if(mode==6||mode==9)require(failure instanceof ClassCastException&&actual==Trace.partial);
            else require(failure==null&&actual==Trace.partial&&value==desired);
            if(mode==1)require(Trace.partial==sentinel&&Trace.result==sentinel);
            else {Target<?> partial=(Target<?>)Trace.partial;require(partial!=previous&&partial.flag==flag&&partial.type==type&&partial.names==names&&Trace.result==desired);previous=partial;}
            if(names!=null&&names.length>0)require(names[0]==(mode==1?initial:changed));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
