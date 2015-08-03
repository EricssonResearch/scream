function plot_all(a,Tmax,Bmax,Cmax)
    T = a(:,1);
    K = 12;
    %Tmax = 100.0;
    figure(1);
    subplot(2,1,1);
    plot(T,a(:,2),T,a(:,3)); 
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 0.5]);grid on;
    title('OWD [s] and OWD trend');
    legend('OWD','OWD trend');
    subplot(2,1,2);
    plot(T,a(:,15));
    set(gca,'FontSize',12);grid on;
    title('RTP queue delay [s]');    
    xlim([0 Tmax]);grid on;
    %axis([0 Tmax 0 1.0]);grid on;
    xlabel('T [s]');
    figure(2);
    subplot(2,1,1);
    plot(T,a(:,8),T,a(:,9),T,a(:,10));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Cmax]);grid on;    
    title('CWND [byte]');    
    legend('CWND','CWND infl. point','Max bytes in flight');
    subplot(2,1,2);
    plot(T,a(:,16),T,a(:,17));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Bmax]);grid on;
    title('Bitrate [bps]');    
    legend('Target bitrate','Target bitrate infl. point');
    xlabel('T [s]');

    
    