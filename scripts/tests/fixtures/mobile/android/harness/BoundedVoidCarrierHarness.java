import api.*;
import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class BoundedVoidCarrierHarness {
    static final class Left extends Bound {}
    static final class Right extends Bound {}
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned bounded void carrier"); }
    static void call(int route,Function factory,Marker first,Function reset,Function release,Function update,Marker second,int flags,int mask) throws Throwable {
        if(route==0)Client.run(factory,first,reset,release,update,second,flags,mask);
        else if(route==1)BoundedVoidCarrierCaller.call(factory,first,reset,release,update,second,flags,mask);
        else if(route==2) {
            Method m=Provider.class.getDeclaredMethod("work",Function.class,Marker.class,Function.class,Function.class,Function.class,Marker.class,int.class,int.class);
            try { m.invoke(null,factory,first,reset,release,update,second,flags,mask); }catch(InvocationTargetException error) { throw error.getCause(); }
        } else MethodHandles.publicLookup().findStatic(Provider.class,"work",MethodType.methodType(void.class,Function.class,Marker.class,Function.class,Function.class,Function.class,Marker.class,int.class,int.class)).invokeExact(factory,first,reset,release,update,second,flags,mask);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method target=Provider.class.getDeclaredMethod("work",Function.class,Marker.class,Function.class,Function.class,Function.class,Marker.class,int.class,int.class);
        require(Modifier.isPublic(target.getModifiers())&&Modifier.isStatic(target.getModifiers())&&target.getReturnType()==void.class&&target.getTypeParameters().length==1&&target.getTypeParameters()[0].getBounds()[0]==Bound.class);
        TypeVariable<?> formal=target.getTypeParameters()[0];Type[] parameters=target.getGenericParameterTypes();
        Type[] producer=((ParameterizedType)parameters[0]).getActualTypeArguments();
        require(((WildcardType)producer[0]).getLowerBounds()[0]==Context.class&&((WildcardType)producer[1]).getUpperBounds()[0]==formal);
        for(int index=2;index<=4;index++) { Type[] callback=((ParameterizedType)parameters[index]).getActualTypeArguments();require(((WildcardType)callback[0]).getLowerBounds()[0]==formal&&callback[1]==Unit.class); }
        int[] lengths={10,1,3,5,7,10,2,4,6,8,2,4,6,8,1,3,5,7,9,10,10,10,10,10};
        int[] stages={1,7,2,8,3,9,4,10,5,6};
        for(int attempt=0;attempt<2;attempt++) {
            Context context=mode==20?null:new Context();Bound value=mode==5?null:attempt==0?new Left():new Right(),replacement=new Right();
            Unit r0=mode==23?null:new Unit(),r1=mode==23?null:new Unit(),r2=mode==23?null:new Unit();
            Marker first=mode==21?null:new Marker(),second=mode==21?null:new Marker();
            int flags=mode==21?Integer.MIN_VALUE:0x71234567+attempt,mask=mode==21?Integer.MAX_VALUE:0x81234567+attempt;
            Function factory=mode==1?null:new Function() { public Object invoke(Object argument) { require(argument==context);Trace.factorySeen=argument;Trace.hit(7);return mode==6?"wrong bound":Trace.factoryResult; } };
            Function reset=mode==2?null:new Function() { public Object invoke(Object argument) { require(argument==value&&Trace.created==value);Trace.resetInput=argument;if(mode==22)Trace.factoryResult=replacement;Trace.hit(8);return mode==7?"wrong unit":r0; } };
            Function release=mode==3?null:new Function() { public Object invoke(Object argument) { require(argument==value&&Trace.created==value);Trace.releaseInput=argument;Trace.hit(9);return mode==8?"wrong unit":r1; } };
            Function update=mode==4?null:new Function() { public Object invoke(Object argument) { require(argument==value&&Trace.created==value);Trace.updateInput=argument;Trace.hit(10);return mode==9?"wrong unit":r2; } };
            Trace.context=context;Trace.factoryResult=value;Trace.failure=new RuntimeException("owned failure");
            Trace.factory=Trace.first=Trace.reset=Trace.release=Trace.update=Trace.second=Trace.factorySeen=Trace.created=Trace.resetInput=Trace.releaseInput=Trace.updateInput=null;
            Trace.resetResult=Trace.releaseResult=Trace.updateResult=null;Trace.events=0;Trace.flags=Trace.mask=0;Trace.done=false;
            Trace.failAt=mode==10?7:mode==11?8:mode==12?9:mode==13?10:mode>=14&&mode<=19?mode-13:0;
            Throwable caught=null;
            try { call(route,factory,first,reset,release,update,second,flags,mask); }catch(Throwable error) { caught=error; }
            if(mode>=1&&mode<=4)require(caught instanceof NullPointerException);else if(mode>=6&&mode<=9)require(caught instanceof ClassCastException);else if(mode>=10&&mode<=19)require(caught==Trace.failure);else require(caught==null);
            int length=lengths[mode];long events=0;for(int i=0;i<length;i++)events=events*100+stages[i];
            require(Trace.events==events&&Trace.factory==factory&&Trace.first==first&&Trace.reset==reset&&Trace.release==release&&Trace.update==update&&Trace.second==second&&Trace.flags==flags&&Trace.mask==mask);
            require(Trace.factorySeen==(length>=2?context:null)&&Trace.created==(length>=3?value:null));
            require(Trace.resetInput==(length>=4?value:null)&&Trace.releaseInput==(length>=6?value:null)&&Trace.updateInput==(length>=8?value:null));
            require(Trace.resetResult==(length>=5?r0:null)&&Trace.releaseResult==(length>=7?r1:null)&&Trace.updateResult==(length>=9?r2:null)&&Trace.done==(length==10));
            require(Trace.factoryResult==(mode==22?replacement:value));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
