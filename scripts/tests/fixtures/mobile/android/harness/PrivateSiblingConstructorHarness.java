import fixture.Core;
import fixture.Probe;
import fixture.Trace;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.util.AbstractList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.function.Supplier;

public final class PrivateSiblingConstructorHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned private sibling constructor");
    }
    static Object field(Object object,String name) {
        try {Field f=object.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(object);}
        catch (ReflectiveOperationException failure) {throw new AssertionError("owned private sibling constructor field",failure);}
    }
    private static final class OpaqueList extends AbstractList<Object> {
        Object element;
        public int size() {throw new AssertionError("owned private sibling constructor inspected list");}
        public Object get(int index) {require(index==0);Trace.hit(6);return element;}
    }
    static Object call(int route,Object context,List<?> values,Probe probe,Constructor<?> access) throws Throwable {
        if (route==0) return Core.create(context,values,probe);
        if (route==1) return PrivateSiblingConstructorCaller.call(context,values,probe);
        if (route==2) {
            try {return access.newInstance(context,values,probe,null);}
            catch (InvocationTargetException failure) {throw failure.getCause();}
        }
        return MethodHandles.publicLookup().findStatic(Core.class,"create",
            MethodType.methodType(Object.class,Object.class,List.class,Probe.class)).invokeExact(context,(List)values,probe);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        final boolean rebuilt=Boolean.parseBoolean(args[3]);
        final Class<?> markerType=Class.forName(args[2]);
        final Class<?> entryType=Class.forName("fixture.Core$Entry"),contextType=Class.forName("fixture.Core$Context"),valueType=Class.forName("fixture.Core$Value");
        for (Class<?> type:new Class<?>[]{entryType,contextType,valueType})
            require(type.getEnclosingClass()==Core.class&&Modifier.isPrivate(type.getModifiers())&&Modifier.isStatic(type.getModifiers()));
        require(Runnable.class.isAssignableFrom(entryType)&&Supplier.class.isAssignableFrom(contextType));
        Constructor<?> target=entryType.getDeclaredConstructor(contextType,List.class,Probe.class);
        Constructor<?> access=entryType.getDeclaredConstructor(contextType,List.class,Probe.class,markerType);
        require(Modifier.isPrivate(target.getModifiers())&&access.isSynthetic()!=rebuilt);
        require(!Modifier.isPublic(access.getModifiers())&&!Modifier.isPrivate(access.getModifiers()));
        require(Arrays.equals(target.getExceptionTypes(),new Class<?>[]{IOException.class})&&Arrays.equals(access.getExceptionTypes(),target.getExceptionTypes()));
        ParameterizedType list=(ParameterizedType)target.getGenericParameterTypes()[1];
        require(list.getRawType()==List.class&&Arrays.equals(list.getActualTypeArguments(),new Class<?>[]{valueType}));
        require(markerType.getEnclosingClass()==Core.class&&markerType.getDeclaredFields().length==0&&markerType.isAnonymousClass()!=rebuilt);
        require(markerType.getDeclaredConstructors().length==(rebuilt?1:0));
        access.setAccessible(true);
        final Object contextPayload=new Object(),valuePayload=new Object(),value=Core.value(valuePayload);
        require(field(value,"payload")==valuePayload);
        final Object context=mode==1?null:Core.context(contextPayload);
        final OpaqueList opaque=new OpaqueList();opaque.element=mode==12?new Object():value;
        final List<?> values=mode==2?null:mode==11?Collections.emptyList():opaque;
        final IOException callbackFailure=new IOException("owned callback failure"),targetFailure=new IOException("owned target failure");
        final RuntimeException runtimeFailure=new IllegalArgumentException("owned callback runtime");
        final Error callbackError=new Error("owned callback error");
        Trace.failure=new IllegalStateException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for (int attempt=0;attempt<2;attempt++) {
            opaque.element=mode==12?new Object():value;
            Trace.events=0;Trace.checked=mode==8||mode==10?targetFailure:null;Core.latest=null;
            final Object marker=mode==13?null:new Object();final int[] calls={0};
            Probe probe=mode==3?null:new Probe() {public Object observe(Object self,Object actualValues,Object actualContext) throws IOException {
                require(++calls[0]==1&&self==Core.latest&&actualValues==values&&actualContext==context);
                require(field(self,"context")==context&&field(self,"values")==values&&((Integer)field(self,"stage"))==1&&field(self,"marker")==null);
                Trace.hit(3);
                if (mode==9) Trace.checked=targetFailure;
                if (mode==10) Trace.checked=null;
                if (mode==15) opaque.element=new Object();
                if (mode==4) throw callbackFailure;
                if (mode==5) throw callbackError;
                if (mode==14) throw runtimeFailure;
                return marker;
            }};
            Object result=null;Throwable failure=null;
            try {result=call(route,context,values,probe,access);} catch (Throwable error) {failure=error;}
            final boolean callbackFailed=mode==3||mode==4||mode==5||mode==6||mode==14;
            final boolean targetFailed=mode==7||mode==8||mode==9;
            final boolean success=!callbackFailed&&!targetFailed;
            final int stage=callbackFailed?1:targetFailed?2:3;
            require(Trace.events==(mode==3||mode==6?1:mode==4||mode==5||mode==8||mode==9||mode==14?13:132));
            Object partial=Core.latest;
            require(partial!=null&&field(partial,"context")==context&&field(partial,"values")==values&&((Integer)field(partial,"stage"))==stage);
            require(field(partial,"marker")== (callbackFailed?null:marker)&&calls[0]==(mode==3||mode==6?0:1));
            require(result==(success?partial:null));
            if (mode==3) require(failure instanceof NullPointerException);
            else if (mode==4) require(failure==callbackFailure);
            else if (mode==5) require(failure==callbackError);
            else if (mode==6||mode==7) require(failure==Trace.failure);
            else if (mode==8||mode==9) require(failure==targetFailure);
            else if (mode==14) require(failure==runtimeFailure);
            else require(failure==null);
            if (success) {
                ((Runnable)result).run();
                if (context!=null) require(((Supplier<?>)context).get()==contextPayload);
                if (values==opaque) {
                    Object element=values.get(0);boolean castFailed=false;
                    try {require(valueType.cast(element)==value);} catch (ClassCastException expected) {castFailed=true;}
                    require(castFailed==(mode==12||mode==15));
                }
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",stage="+stage);
        }
    }
}
