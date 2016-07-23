function test_v_a(a,Tmax,I,Bmax,Cmax)
    T = a(:,1);
    K = 12;
    %Tmax = 100.0;

        
    figure(1);
    subplot(2,1,1);
    plot(T,a(:,2),T,a(:,3)); 
    set(gca,'FontSize',12);grid on;
    set(gca,'XTickLabel',[]);    
    axis([0 Tmax 0 0.3]);grid on;
    title('qdel[s] and qdel trend');
    %legend('OWD','OWD trend');
    subplot(2,1,2);
    plot(T,a(:,10),T,a(:,8));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Cmax]);grid on;    
    title('CWND & in flight [byte]');    
    xlabel('T [s]');

    figure(2);
    subplot(2,1,1);
    plot(T,a(:,14)); 
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 0.1]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay');
    subplot(2,1,2);
    plot(T,a(:,15),T,a(:,16),T,a(:,17),T,a(:,18));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Bmax(1)]);grid on;
    title('Bitrate [bps]');    
    legend('Target','TargetI','Transmitted', 'Acked');
    xlabel('T [s]');
    
    if I==1
    figure(3);
    subplot(2,1,1);
    plot(T,a(:,14+7)); 
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 0.1]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay');
    subplot(2,1,2);
    plot(T,a(:,15+7),T,a(:,16+7),T,a(:,17+7),T,a(:,18+7));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Bmax(2)]);grid on;
    title('Bitrate [bps]');    
    legend('Target','TargetI','Transmitted', 'Acked');
    xlabel('T [s]');
    end
    