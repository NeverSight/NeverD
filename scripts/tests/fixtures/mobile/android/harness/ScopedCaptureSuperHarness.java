import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ScopedCaptureSuperHarness {
    static void require(boolean ok) {if(!ok)throw new AssertionError("owned scoped capture super");}
    static Child<?> call(int route,int kind,Class<String> type,String[] names) throws Throwable {
        boolean flag=kind==2;
        if(route==0)return kind==0?new Child<String>(type,names):new Child<String>(flag,type,names);
        if(route==1)return kind==0?ScopedCaptureSuperCaller.defaults(type,names):ScopedCaptureSuperCaller.direct(flag,type,names);
        if(route==2) {
            try {return (Child<?>)(kind==0?Child.class.getConstructor(Class.class,String[].class).newInstance(type,names):Child.class.getConstructor(boolean.class,Class.class,String[].class).newInstance(flag,type,names));}
            catch(InvocationTargetException failure) {throw failure.getCause();}
        }
        if(kind==0)return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,Class.class,String[].class)).invokeExact((Class)type,names);
        return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,boolean.class,Class.class,String[].class)).invokeExact(flag,(Class)type,names);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final boolean flag=kind==2;final Class<String> type=mode==10?null:String.class;
        final String initial=new String("owned initial"),captured=new String("owned captured"),stored=new String("owned stored");
        final String[] names=mode==11?null:mode==9?new String[0]:new String[]{initial,initial};
        final Object left=new Object(),right=new Object();
        final RuntimeException proxyFailure=new RuntimeException("owned proxy failure"),parentFailure=new RuntimeException("owned parent failure"),comparisonFailure=new RuntimeException("owned comparison failure");
        final Error parentError=new Error("owned parent error");
        final Object desired=mode==9?new String("owned wrong result"):mode==10?null:Integer.valueOf(Integer.MIN_VALUE+37+mode);
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==1?1:mode==3?2:mode==4?3:mode==7?4:0;
        Object previousProxy=null,previousPartial=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),oldProxy=previousProxy,oldPartial=previousPartial;
            Trace.events=0;Trace.proxy=Trace.partial=Trace.left=Trace.right=Trace.result=sentinel;int created=Trace.created,parents=Trace.parents;
            if(names!=null&&names.length>0)names[0]=initial;
            Trace.proxyAction=new Runnable(){public void run(){
                require(Trace.events==1&&Trace.proxy instanceof fixture.Proxy&&Trace.proxy!=oldProxy&&Trace.partial==sentinel);
                fixture.Proxy proxy=(fixture.Proxy)Trace.proxy;require(proxy.names==names&&proxy.type==type&&proxy.tag!=null);
                Trace.hit(6);if(names!=null&&names.length>0){require(names[0]==initial);names[0]=captured;}
                if(mode==2)throw proxyFailure;
            }};
            Trace.action=new Runnable(){public void run(){
                require(Trace.events==1623&&Trace.partial instanceof Child&&Trace.partial!=oldPartial);
                Parent<?> partial=(Parent<?>)Trace.partial;require(partial.flag==flag&&(Object)partial.cmp==Trace.proxy);
                Trace.hit(7);if(names!=null&&names.length>0){require(names[0]==captured);names[0]=stored;}
                if(mode==5)throw parentFailure;if(mode==6)throw parentError;
            }};
            Trace.compareAction=new Runnable(){public void run(){
                require(Trace.events==1623745&&Trace.left==left&&Trace.right==right);
                Trace.hit(8);Trace.result=desired;if(mode==8)throw comparisonFailure;
            }};
            Child<?> actual=null;Throwable failure=null;int answer=42;
            try{actual=call(route,kind,type,names);answer=((Comparator)actual.cmp).compare(left,right);}catch(Throwable error){failure=error;}
            boolean proxyFailed=mode==1||mode==2||mode==3,parentFailed=mode==4||mode==5||mode==6||mode==7;
            long events=mode==1?1:mode==2?16:mode==3?162:mode==4?1623:mode==5||mode==6?16237:mode==7?162374:16237458;
            require(Trace.events==events&&Trace.created==created+1&&Trace.parents==parents+(proxyFailed?0:1));
            Throwable expected=mode==1||mode==3||mode==4||mode==7?Trace.failure:mode==2?proxyFailure:mode==5?parentFailure:mode==6?parentError:mode==8?comparisonFailure:null;
            if(expected!=null)require(failure==expected&&actual==(proxyFailed||parentFailed?null:Trace.partial)&&answer==42);
            else if(mode==9)require(failure instanceof ClassCastException&&actual==Trace.partial&&answer==42);
            else if(mode==10)require(failure instanceof NullPointerException&&actual==Trace.partial&&answer==42);
            else require(failure==null&&actual==Trace.partial&&answer==((Integer)desired).intValue());
            if(mode==1)require(Trace.proxy==sentinel);else {fixture.Proxy proxy=(fixture.Proxy)Trace.proxy;require(proxy!=previousProxy&&proxy.type==type&&proxy.names==names);previousProxy=proxy;}
            if(proxyFailed||mode==4)require(Trace.partial==sentinel);else {Parent<?> partial=(Parent<?>)Trace.partial;require(partial!=previousPartial&&partial.flag==flag&&(Object)partial.cmp==Trace.proxy);previousPartial=partial;}
            if(names!=null&&names.length>0)require(names[0]==(mode==1?initial:proxyFailed||mode==4?captured:stored));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
