import fixture.Core;
import fixture.Parent;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class BoundNominalSuperHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned bound nominal super mismatch");
    }
    private static Core call(int route, int operation, String a, String b, int x, int y,
                             Integer first, Integer second, long tag) throws Throwable {
        if (route == 0) return operation == 0 ? new Core(a,b,x,y,tag) : new Core(a,b,first,second,tag);
        if (route == 1) return operation == 0 ? BoundNominalSuperCaller.made(a,b,x,y,tag) : BoundNominalSuperCaller.direct(a,b,first,second,tag);
        if (route == 2) {
            try {
                if (operation == 0) return Core.class.getConstructor(String.class,String.class,int.class,int.class,long.class).newInstance(a,b,x,y,tag);
                return Core.class.getConstructor(String.class,String.class,Integer.class,Integer.class,long.class).newInstance(a,b,first,second,tag);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 0) return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class,String.class,String.class,int.class,int.class,long.class)).invokeExact(a,b,x,y,tag);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,
            MethodType.methodType(void.class,String.class,String.class,Integer.class,Integer.class,long.class)).invokeExact(a,b,first,second,tag);
    }
    @SuppressWarnings({"unchecked", "rawtypes", "removal"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]), operation=Integer.parseInt(args[1]), mode=Integer.parseInt(args[2]);
        final String left=mode==1?null:new String("left"), right=mode==4?left:new String("right");
        final int x=Integer.MIN_VALUE+17+mode, y=Integer.MAX_VALUE-31-mode;
        final long tag=mode%2==0?Long.MIN_VALUE+mode:Long.MAX_VALUE-mode;
        final Integer first=(mode==1||mode==3)?null:new Integer(1000+mode);
        final Integer second=(mode==2||mode==3)?null:mode==4?first:new Integer(-2000-mode);
        final Integer replacementFirst=new Integer(41), replacementSecond=new Integer(53);
        final Double late=new Double(3.5);
        final RuntimeException actionFailure=new IllegalArgumentException("owned action");
        final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalStateException("owned stage");
        Trace.failAt=mode==5?(operation==0?1:3):mode==6?(operation==0?2:5):mode==7?3:mode==10?5:mode==11?6:0;
        for (int attempt=0; attempt<2; attempt++) {
            Trace.events=0;Trace.calls=0;Trace.inFirst=42;Trace.inSecond=43;
            Trace.first=first;Trace.second=second;Trace.partial=null;
            Trace.action=new Runnable() { public void run() {
                Trace.hit(4);
                Parent<?> partial=(Parent<?>)Trace.partial;
                require(partial.first==first&&partial.second==second&&partial.tag==tag);
                Core child=(Core)partial;
                require(child.left==null&&child.right==null&&child.originalFirst==0&&child.originalSecond==0);
                require(Trace.calls==(operation==0?2:0));
                if(mode==8)throw actionFailure;
                if(mode==9)throw actionError;
                if(mode==12)((Parent)partial).first=late;
                if(mode==13){Trace.first=replacementFirst;Trace.second=replacementSecond;}
            }};
            Object sentinel=new Object(), observed=sentinel, checked=sentinel;
            Core result=null;Throwable failure=null;
            try {
                result=call(route,operation,left,right,x,y,first,second,tag);
                Trace.hit(7);
                observed=((Parent<?>)result).first;
                checked=(Integer)observed;
            } catch(Throwable error){failure=error;}
            String sequence=operation==0?"1234567":"34567";
            int stop=Trace.failAt!=0?Trace.failAt:mode==8||mode==9?4:7;
            long expected=Long.parseLong(sequence.substring(0,sequence.indexOf((char)('0'+stop))+1));
            require(Trace.events==expected);
            int calls=operation==1?0:stop==1?1:2;
            require(Trace.calls==calls&&Trace.inFirst==(calls>0?x:42)&&Trace.inSecond==(calls>1?y:43));
            Throwable exact=Trace.failAt!=0?Trace.failure:mode==8?actionFailure:mode==9?actionError:null;
            if(exact!=null)require(failure==exact&&result==null&&observed==sentinel&&checked==sentinel);
            else if(mode==12)require(failure instanceof ClassCastException&&result!=null&&observed==late&&checked==sentinel);
            else require(failure==null&&result!=null&&observed==first&&checked==first);
            if(stop<=3)require(Trace.partial==null);
            else {
                Parent<?> partial=(Parent<?>)Trace.partial;
                require(partial.first==(mode==12?(Object)late:first)&&partial.second==second&&partial.tag==tag);
                Core child=(Core)partial;
                boolean fields=stop>=6;
                require(child.left==(fields?left:null)&&child.right==(fields?right:null));
                require(child.originalFirst==(fields&&operation==0?x:0)&&child.originalSecond==(fields&&operation==0?y:0));
                if(result!=null)require(result==partial);
            }
            require(Trace.first==(mode==13?replacementFirst:first)&&Trace.second==(mode==13?replacementSecond:second));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
