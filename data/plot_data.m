%% --- Load Data ---
file = 'linklite_data_4.csv';
data = readmatrix(file);                          % 20x9 numeric matrix
hdr  = readcell(file, 'Range', '1:1');            % header row (cell array of strings)

brightness = 1;        % (%)
scaled_duty = 3;       % (%)
measured_duty = 4;     % (%)

total_current = 5;     % mA
power_consumption = 6; % W
voltage_drop = 7;      % V
voltage_rail = 8;      % V
led_current = 9;       % mA

efficiency = ((data(:,led_current) * 28) .* data(:,voltage_rail)) ./ ...
             ((data(:,total_current) - 72) * 15); % (%)

buck_output = efficiency .* data(:,total_current) .* 15 ./ data(:,voltage_rail)

%% graphing data

figure;

sgtitle("Linklite Power Metrics", 'fontsize', 44);

subplot(2,2,1);
plot(data(:,brightness), data(:,power_consumption), '-c', 'LineWidth', 2.5, 'MarkerSize', 10, 'Color', 'g');
title("Power Consumption (W) vs. Brightness (%)", 'FontSize', 28)
xlim([0 100]);
grid on;

subplot(2,2,2);
plot(data(:,brightness), data(:,led_current), 'LineWidth', 2.5, 'Color', 'r');
xlim([0 100]);
ylim([0 60]);
title("LED Forward Current (V) vs. Bightness (%)", 'FontSize', 28);
grid on;

subplot(2,2,3);
plot(data(:,brightness), efficiency, 'LineWidth', 2.5, 'Color', 'y');
xlim([0 100]);
ylim([0 1]);
title("Calculated Buck Efficiency (%) vs. Brightness (%)", 'FontSize', 28);
grid on;

subplot(2,2,4);
plot(data(:,brightness), buck_output, 'LineWidth', 2.5, 'color', 'c');
xlim([0 100]);
ylim([0 2000]);
title("Buck Output (mA) vs. Brightness (%)", 'FontSize', 28);
grid on;

for k = 1:4
    subplot(2,2,k);
    % ... your plot, title, labels ...
    set(gca, 'FontSize', 20);
end

%% --- Load and Graph Experimental Data ---

figure

hold on

temp_file = 'files/data_006.csv';
temp_data = readmatrix(temp_file);                          
plot(temp_data(:,1),temp_data(:,2),'LineWidth',2);

temp_file = 'files/data_010.csv';
temp_data = readmatrix(temp_file);                          
plot(temp_data(:,1),temp_data(:,2),'LineWidth',2);

temp_file = 'files/data_008.csv';
temp_data = readmatrix(temp_file);                          
plot(temp_data(:,1),temp_data(:,2),'LineWidth',2);

temp_file = 'files/data_009.csv';
temp_data = readmatrix(temp_file);                          
plot(temp_data(:,1),temp_data(:,2),'LineWidth',2);

temp_data_1 = readmatrix('files/data_011.csv');
temp_data_2 = readmatrix('files/data_012.csv');
temp_data_3 = readmatrix('files/data_013.csv');

% shortest row count across the three
n = min([size(temp_data_1,1), size(temp_data_2,1), size(temp_data_3,1)]);

% trim each to n rows
d1 = temp_data_1(1:n, :);
d2 = temp_data_2(1:n, :);
d3 = temp_data_3(1:n, :);

averaged_temp = (d1(:,1) + d2(:,1) + d3(:,1)) ./ 3;
plot(averaged_temp, d1(:,2));

temp_file = 'files/data_014.csv';
temp_data = readmatrix(temp_file);                          
plot(temp_data(:,1),temp_data(:,2),'LineWidth',2);

hold off
grid on

set(gca, 'FontSize', 24);

xticks(0:300:1800);
xlim([0 1800]);

yticks(20:5:70);
ylim([20 70]);

xlabel("Time (s)", FontSize=28);
ylabel("Temperature (C)", FontSize=28);
title("LED-Side PCB Temperature (Celsius) vs. Time (Seconds)",FontSize=38);

legend( ...
    'Single 90% (014)', ...
    'Single 80% (006)', ...
    'Single 80% (010)', ...
    'Double 80% (008)', ...
    'Triple 80% (009)', ...
    'Single 60% (011/012/013)', ...
    'fontsize',28, ...
    'Location','Southeast' ...
    );


