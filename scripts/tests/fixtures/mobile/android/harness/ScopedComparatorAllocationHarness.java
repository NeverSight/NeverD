import fixture.*;
import java.util.Comparator;
import java.util.Map;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ScopedComparatorAllocationHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped comparator allocation");}
    private static final class Compare implements Comparator<Object> {
        public int compare(Object left,Object right){Trace.compares++;Trace.left=left;Trace.right=right;int result=((String)left).compareTo((String)right);Trace.compareAction.run();return result;}
    }
    @SuppressWarnings("unchecked")
    private static Core<String,Object> call(int route,int operation,Comparator<Object> value)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(value):new Core<String,Object>(value,7);
        if(route==1)return operation==0?ScopedComparatorAllocationCaller.first(value):ScopedComparatorAllocationCaller.second(value);
        if(route==2){try{return operation==0?Core.class.getConstructor(Comparator.class).newInstance(value):Core.class.getConstructor(Comparator.class,int.class).newInstance(value,7);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Comparator.class)).invokeExact(value);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Comparator.class,int.class)).invokeExact(value,7);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Comparator<Object> comparator=mode==1?null:new Compare(),other=new Compare();
        final RuntimeException runtime=new IllegalArgumentException("owned action");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),allocated=new Object(),parented=new Object(),first=new Object(),second=new Object();Trace.events=0;Trace.created=null;Trace.selected=null;Trace.parent=null;Trace.capturedComparator=null;Trace.dynamicComparator=comparator;Trace.left=null;Trace.right=null;Trace.allocationPayload=sentinel;Trace.payload=sentinel;Trace.key=mode==10?Integer.valueOf(3):mode==11?null:new String("owned-key");
            final int defaults=Trace.defaults,parents=Trace.parents,actions=Trace.actions,allocations=Trace.allocations,compares=Trace.compares,wrong=Trace.wrong;
            Trace.allocateAction=new Runnable(){public void run(){Trace.allocations++;require(Trace.events==1&&Trace.created!=null&&Trace.parent==null&&Trace.capturedComparator==comparator&&((Fresh)Trace.created).comparator==comparator);Trace.allocationPayload=allocated;Trace.dynamicComparator=other;if(mode==2)throw runtime;}};
            Trace.compareAction=new Runnable(){public void run(){require(Trace.events==12&&Trace.selected==Trace.created&&Trace.parent!=null&&Trace.parent.value==Trace.created);if(Trace.compares==compares+2){require(Trace.left.equals("b")&&Trace.right.equals("a"));if(mode==4)throw runtime;if(mode==5)throw error;}else require(Trace.compares==compares+1&&Trace.left.equals("a")&&Trace.right.equals("a"));}};
            Trace.parentAction=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.parent.value==Trace.created&&Trace.selected==Trace.created&&Trace.dynamicComparator==other&&Trace.capturedComparator==comparator&&((Fresh)Trace.created).comparator==comparator);Trace.payload=parented;if(mode==3)throw error;Map map=(Map)Trace.created;if(comparator!=null)((Comparator)((Fresh)Trace.created).comparator).compare("a","a");map.put("a",first);if(comparator!=null)((Comparator)((Fresh)Trace.created).comparator).compare("b","a");map.put("b",second);}};
            Core<String,Object> result=null;String key=null;Throwable failure=null;try{result=call(route,operation,comparator);key=result.key();int length=key.length();require(length==9);}catch(Throwable caught){failure=caught;}
            boolean allocationActionReached=mode!=6,parentReached=mode!=2&&mode!=6,parentActionReached=parentReached&&mode!=7,putsReached=parentActionReached&&mode!=3;
            long events=mode==2||mode==6?1:mode==3||mode==4||mode==5||mode==7?12:mode==8?123:1234;
            require(Trace.events==events&&Trace.defaults==defaults+1&&Trace.allocations==allocations+(allocationActionReached?1:0)&&Trace.parents==parents+(parentReached?1:0)&&Trace.actions==actions+(parentActionReached?1:0)&&Trace.compares==compares+(putsReached&&comparator!=null?2:0)&&Trace.wrong==wrong);
            Throwable expected=mode==2||mode==4?runtime:mode==3||mode==5?error:mode>=6&&mode<=9?Trace.failure:null;
            if(expected!=null)require(failure==expected&&result==null);
            else if(mode==10)require(failure instanceof ClassCastException&&result!=null);
            else if(mode==11)require(failure instanceof NullPointerException&&result!=null&&key==null);
            else require(failure==null&&result!=null&&key==Trace.key);
            require(Trace.created!=null&&Trace.capturedComparator==comparator&&((Fresh)Trace.created).comparator==comparator&&Trace.payload==(parentActionReached?parented:sentinel)&&Trace.allocationPayload==(allocationActionReached?allocated:sentinel));
            require((Trace.parent!=null)==parentActionReached);
            Object[] values=Trace.created.values().toArray();int size=putsReached?(mode==4||mode==5?1:2):0;require(values.length==size);if(size==1)require(values[0]==first);if(size==2)require(values[0]==first&&values[1]==second||values[0]==second&&values[1]==first);
            if(parentActionReached)require(Trace.parent.value==Trace.created&&Trace.selected==Trace.created);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
